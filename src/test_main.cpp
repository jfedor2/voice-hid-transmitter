// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Command line tool for exercising the pieces without the UI or a microphone.
//
//   vht-test notation "<text>" ...
//       Parses notation and prints the canonical form (or the error).
//   vht-test recognize <stream.wav> <name>[:continuous][:<threshold>]=<a.wav>,<b.wav>,... ...
//       Trains sounds from WAV files (16 kHz mono 16-bit), feeds the stream
//       through the recognizer in 10 ms chunks and prints what it detects.
//   vht-test record <stream.wav> <out_prefix>
//       Runs the stream through recording mode and writes the recorded clips.
//   vht-test eval <list.txt> <train sessions> <test sessions> [threshold] [continuous threshold] [cache dir]
//       Trains on some sessions of a labeled set of clips, tests on others.
//   vht-test embed <in.wav> <out.bin>
//       Writes the mel spectrogram and first embedding, for checking the model.
//   vht-test background <out.bin> <a.wav> ...
//       Builds the background file from sounds that aren't commands.
//   vht-test vectors <out.bin> <a.wav> ...
//       Writes the embeddings, statistics and vectors of the files.
//   vht-test engine "<notation>" [start|fire]
//       Runs a program through the engine (no receiver) and prints the state.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "embedding.h"
#include "engine.h"
#include "notation.h"
#include "recognizer.h"
#include "sound_vectors.h"
#include "util.h"

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) {
            return out;
        }
        start = p + 1;
    }
}

static int cmd_notation(int argc, char** argv) {
    for (int i = 2; i < argc; i++) {
        Program p;
        std::string error;
        if (parse_program(argv[i], p, error)) {
            printf("OK    %-40s -> %s\n", argv[i], format_program(p).c_str());
        } else {
            printf("ERROR %-40s -> %s\n", argv[i], error.c_str());
        }
    }
    return 0;
}

static int cmd_recognize(int argc, char** argv) {
    if (argc < 4) {
        return 1;
    }
    Audio stream;
    if (!read_wav(from_utf8(argv[2]), stream)) {
        fprintf(stderr, "can't read %s\n", argv[2]);
        return 1;
    }
    std::vector<SoundSpec> specs;
    std::vector<std::string> names = { "" };
    uint64_t next_id = 1;
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            return 1;
        }
        std::vector<std::string> head = split(arg.substr(0, eq), ':');
        SoundSpec spec;
        spec.action_id = (uint32_t) names.size();
        names.push_back(head[0]);
        for (size_t h = 1; h < head.size(); h++) {
            if (head[h] == "continuous") {
                spec.mode = SoundMode::Continuous;
            } else {
                spec.threshold = (float) atof(head[h].c_str());
            }
        }

        for (const std::string& file : split(arg.substr(eq + 1), ',')) {
            auto audio = std::make_shared<Audio>();
            if (!read_wav(from_utf8(file), *audio)) {
                fprintf(stderr, "can't read %s\n", file.c_str());
                return 1;
            }
            spec.samples.push_back({ next_id++, audio });
        }
        specs.push_back(spec);
    }

    Recognizer rec;
    uint64_t frames_done = 0;
    rec.set_callback([&](const Detection& d) {
        static const char* types[] = { "FIRE ", "START", "STOP " };
        printf("%8.2f s  %s  %-12s score %.2f\n", frames_done * HOP / (double) SAMPLE_RATE, types[(int) d.type],
            names[d.action_id].c_str(), d.score);
    });
    rec.set_sounds(specs);
    while (rec.building()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (const auto& [id, info] : rec.sound_info()) {
        printf("sound %-12s samples %d  most similar: %s (%d confused)\n", names[id].c_str(), info.samples,
            info.similar_action ? names[info.similar_action].c_str() : "-", info.confused_samples);
    }

    auto t0 = std::chrono::steady_clock::now();
    uint64_t last_heard = 0;
    for (size_t pos = 0; pos + HOP <= stream.size(); pos += HOP) {
        rec.process(&stream[pos], HOP);
        frames_done++;
        RecognizerStatus st = rec.status();
        if (st.sounds_heard != last_heard) {
            last_heard = st.sounds_heard;
            printf("%8.2f s  (sound ended; floor %.0f dB, scores:", frames_done * HOP / (double) SAMPLE_RATE, st.floor_db);
            for (const auto& [id, score] : st.scores) {
                printf(" %s=%.2f", names[id].c_str(), score);
            }
            printf(")\n");
        }
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    printf("processed %.1f s of audio in %lld ms (%.2f%% of real time)\n", stream.size() / (double) SAMPLE_RATE,
        (long long) ms, 100.0 * ms / (stream.size() * 1000.0 / SAMPLE_RATE));
    return 0;
}

// Evaluates the recognizer on a labeled set of clips. The list file has
// tab-separated lines "<label> <session> <path> [continuous]" (labels
// starting with "-" = not a command sound). Sounds are trained on the clips of
// the train sessions; each clip of the test sessions is then streamed through
// the recognizer (with quiet around it). Prints one line per test clip:
// label, path, what fired (or "-"), its score, the best scoring action and its
// score, and the events ("F:name@ms", "S:..." start, "E:..." stop; ms from the
// start of the clip).
static int cmd_eval(int argc, char** argv) {
    if (argc < 5) {
        return 1;
    }
    struct Item {
        std::string label, session, path;
        std::shared_ptr<Audio> audio;
        bool continuous;
    };
    std::vector<Item> items;
    FILE* f = fopen(argv[2], "r");
    if (f == nullptr) {
        return 1;
    }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::vector<std::string> parts = split(std::string(line).substr(0, strcspn(line, "\r\n")), '\t');
        if (parts.size() < 3) {
            continue;
        }
        Item it{ parts[0], parts[1], parts[2], std::make_shared<Audio>(), parts.size() > 3 && parts[3] == "continuous" };
        if (!read_wav(from_utf8(it.path), *it.audio)) {
            fprintf(stderr, "can't read %s\n", it.path.c_str());
            return 1;
        }
        items.push_back(it);
    }
    fclose(f);
    const std::vector<std::string> train = split(argv[3], ','), test = split(argv[4], ',');
    const float threshold = argc > 5 ? (float) atof(argv[5]) : 0.0f;
    const float continuous_threshold = argc > 6 ? (float) atof(argv[6]) : 0.0f;
    auto in = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    std::vector<std::string> names = { "" };
    std::vector<SoundSpec> specs;
    uint64_t next_id = 1;
    for (const Item& it : items) {
        if (it.label[0] == '-' || !in(train, it.session)) {
            continue;
        }
        auto pos = std::find(names.begin(), names.end(), it.label);
        if (pos == names.end()) {
            names.push_back(it.label);
            SoundSpec spec;
            spec.action_id = (uint32_t) names.size() - 1;
            spec.mode = it.continuous ? SoundMode::Continuous : SoundMode::Whole;
            spec.threshold = it.continuous ? continuous_threshold : threshold;
            specs.push_back(spec);
            pos = names.end() - 1;
        }
        specs[pos - names.begin() - 1].samples.push_back({ next_id++, it.audio });
    }

    Recognizer rec;
    if (argc > 7) {
        rec.set_cache_dir(from_utf8(argv[7]));
    }
    std::string fired;
    float fired_score = 0;
    std::string events;
    size_t clip_pos = 0;  // samples into the clip
    rec.set_callback([&](const Detection& d) {
        if (d.type != TriggerType::Stop && d.score > fired_score) {
            fired = names[d.action_id];
            fired_score = d.score;
        }
        static const char* const types[] = { "F", "S", "E" };
        char e[128];
        snprintf(e, sizeof(e), "%s%s:%s@%d", events.empty() ? "" : ";", types[(int) d.type], names[d.action_id].c_str(),
            (int) ((long long) clip_pos * 1000 / SAMPLE_RATE));
        events += e;
    });
    rec.set_sounds(specs);
    while (rec.building()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Quiet background, about -80 dBFS.
    uint32_t seed = 1;
    Audio pad(SAMPLE_RATE * 6 / 10);
    for (float& s : pad) {
        seed = seed * 1664525u + 1013904223u;
        s = ((seed >> 8) / 16777216.0f - 0.5f) * 0.0003f;
    }
    for (const Item& it : items) {
        if (!in(test, it.session)) {
            continue;
        }
        fired.clear();
        fired_score = 0;
        events.clear();
        std::map<uint32_t, float> best;
        Audio stream = pad;
        stream.insert(stream.end(), it.audio->begin(), it.audio->end());
        stream.insert(stream.end(), pad.begin(), pad.end());
        for (size_t pos = 0; pos + HOP <= stream.size(); pos += HOP) {
            clip_pos = pos >= pad.size() ? pos - pad.size() : 0;
            rec.process(&stream[pos], HOP);
            for (const auto& [id, score] : rec.status().scores) {
                best[id] = std::max(best[id], score);
            }
        }
        uint32_t best_id = 0;
        for (const auto& [id, score] : best) {
            if (best_id == 0 || score > best[best_id]) {
                best_id = id;
            }
        }
        printf("%s\t%s\t%s\t%.3f\t%s\t%.3f\t%s\n", it.label.c_str(), it.path.c_str(), fired.empty() ? "-" : fired.c_str(),
            fired_score, best_id ? names[best_id].c_str() : "-", best_id ? best[best_id] : 0.0f,
            events.empty() ? "-" : events.c_str());
    }
    return 0;
}

// Writes the mel spectrogram of a WAV file (frames x 32 floats, no padding)
// and the embedding of its first 76 frames, for checking against the
// original model.
static int cmd_embed(int argc, char** argv) {
    if (argc < 4) {
        return 1;
    }
    Audio audio;
    if (!read_wav(from_utf8(argv[2]), audio)) {
        return 1;
    }
    const speech::Embedder& e = speech::embedder();
    if (!e.loaded()) {
        fprintf(stderr, "model not loaded\n");
        return 1;
    }
    const std::vector<float> mel = e.mel(audio.data(), audio.size());
    FILE* f = fopen(argv[3], "wb");
    const uint32_t frames = (uint32_t) (mel.size() / speech::MEL_BINS);
    fwrite(&frames, 4, 1, f);
    fwrite(mel.data(), 4, mel.size(), f);
    if (frames >= speech::EMBED_FRAMES) {
        const speech::Embedding emb = e.embed(mel.data());
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) {
            e.embed(mel.data());
        }
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 20;
        fwrite(emb.data(), 4, emb.size(), f);
        printf("%u frames, embedding took %lld us\n", frames, (long long) us);
    }
    fclose(f);
    return 0;
}

// Writes the raw description of WAV files (per file: uint32 window count,
// embeddings, then the cepstral statistics), and the normalized vectors.
static int cmd_vectors(int argc, char** argv) {
    if (argc < 4) {
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    for (int i = 3; i < argc; i++) {
        Audio audio;
        if (!read_wav(from_utf8(argv[i]), audio)) {
            return 1;
        }
        const SoundDescription d = describe_sound(audio);
        const std::vector<SoundVector> v = background().vectors(d);
        const uint32_t n = (uint32_t) d.windows.size();
        fwrite(&n, 4, 1, f);
        for (const auto& e : d.windows) {
            fwrite(e.data(), 4, e.size(), f);
        }
        fwrite(d.stats.data(), 4, d.stats.size(), f);
        for (const auto& x : v) {
            fwrite(x.data(), 4, x.size(), f);
        }
    }
    fclose(f);
    return 0;
}

// Builds the background file (models/sound_background.bin) from WAV files of
// things that aren't commands, plus synthetic noises.
static int cmd_background(int argc, char** argv) {
    if (argc < 4) {
        return 1;
    }
    std::vector<Audio> clips;
    for (int i = 3; i < argc; i++) {
        Audio audio;
        if (!read_wav(from_utf8(argv[i]), audio)) {
            fprintf(stderr, "can't read %s\n", argv[i]);
            return 1;
        }
        clips.push_back(std::move(audio));
    }
    const size_t files = clips.size();
    for (Audio& noise : synthetic_noises()) {
        clips.push_back(std::move(noise));
    }
    // Whole sounds, and the starts of them (what a continuous sound is
    // decided on) for continuous sounds.
    std::vector<SoundDescription> sounds(clips.size());
    std::vector<std::vector<SoundDescription>> chunks(clips.size());
    std::vector<std::thread> threads;
    std::atomic<size_t> next{ 0 };
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++) {
        threads.emplace_back([&]() {
            for (size_t i; (i = next++) < clips.size();) {
                sounds[i] = describe_sound(clips[i]);
                for (const Audio& c : sound_chunks(clips[i], 400)) {
                    chunks[i].push_back(describe_chunk(c));
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    std::vector<SoundDescription> all_chunks;
    for (const auto& c : chunks) {
        all_chunks.insert(all_chunks.end(), c.begin(), c.end());
    }
    const std::vector<uint8_t> data = Background::build(sounds, all_chunks);
    FILE* f = fopen(argv[2], "wb");
    if (f == nullptr) {
        return 1;
    }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    printf("%zu files + %zu noises, %zu chunks, %zu bytes\n", files, clips.size() - files, all_chunks.size(), data.size());
    return 0;
}

static int cmd_record(int argc, char** argv) {
    if (argc < 4) {
        return 1;
    }
    Audio stream;
    if (!read_wav(from_utf8(argv[2]), stream)) {
        fprintf(stderr, "can't read %s\n", argv[2]);
        return 1;
    }
    Recognizer rec;
    rec.start_recording();
    for (size_t pos = 0; pos + HOP <= stream.size(); pos += HOP) {
        rec.process(&stream[pos], HOP);
    }
    int n = 0;
    for (const RecordedClip& clip : rec.take_recorded()) {
        std::string path = std::string(argv[3]) + "_" + std::to_string(n++) + ".wav";
        write_wav(from_utf8(path), clip.audio);
        printf("%s  %.2f s (loud %.2f-%.2f s)\n", path.c_str(), clip.audio.size() / (double) SAMPLE_RATE,
            clip.loud_start_ms / 1000.0, clip.loud_end_ms / 1000.0);
    }
    return 0;
}

static int cmd_engine(int argc, char** argv) {
    if (argc < 3) {
        return 1;
    }
    Program p;
    std::string error;
    if (!parse_program(argv[2], p, error)) {
        printf("parse error: %s\n", error.c_str());
        return 1;
    }
    const bool start = argc > 3 && std::string(argv[3]) == "start";
    ActionEngine engine;
    engine.set_actions({ { 1, p } });
    engine.set_active(true);
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine.post({ 1, start ? TriggerType::Start : TriggerType::Fire });
    GamepadState last;
    bool first = true;
    auto t0 = std::chrono::steady_clock::now();
    bool stopped = false;
    for (int i = 0; i < 400; i++) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (start && !stopped && elapsed >= 1000) {
            printf("%5lld ms  (sound stops)\n", (long long) elapsed);
            engine.post({ 1, TriggerType::Stop });
            stopped = true;
        }
        GamepadState s = engine.current_state();
        if (first || s != last) {
            printf("%5lld ms  buttons %05x  LS %+.2f %+.2f  RS %+.2f %+.2f  L2 %.2f R2 %.2f\n", (long long) elapsed,
                s.buttons, s.sticks[0][0], s.sticks[0][1], s.sticks[1][0], s.sticks[1][1], s.triggers[0], s.triggers[1]);
            last = s;
            first = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.stop();
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2) {
        std::string cmd = argv[1];
        if (cmd == "notation") return cmd_notation(argc, argv);
        if (cmd == "recognize") return cmd_recognize(argc, argv);
        if (cmd == "engine") return cmd_engine(argc, argv);
        if (cmd == "record") return cmd_record(argc, argv);
        if (cmd == "eval") return cmd_eval(argc, argv);
        if (cmd == "embed") return cmd_embed(argc, argv);
        if (cmd == "background") return cmd_background(argc, argv);
        if (cmd == "vectors") return cmd_vectors(argc, argv);
    }
    fprintf(stderr, "usage: see the top of test_main.cpp\n");
    return 1;
}
