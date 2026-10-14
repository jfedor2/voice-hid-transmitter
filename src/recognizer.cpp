// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "recognizer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

#include "sound_vectors.h"

namespace {

constexpr int PREROLL_FRAMES = 3;     // kept before the gate opens
constexpr int TAIL_FRAMES = 2;        // kept after the last loud frame
constexpr int MAX_SEG_FRAMES = 400;   // frames kept for matching (longer sounds aren't whole sounds)
constexpr int MAX_WHOLE_FRAMES = 250; // longest sound matched as a whole sound
constexpr int MIN_WHOLE_FRAMES = 2;
constexpr int MAX_RECORD_FRAMES = 800;
constexpr size_t MAX_AUDIO = (MAX_RECORD_FRAMES + 50) * HOP;  // audio kept for one sound
constexpr float ABS_MIN_DB = -65.0f;  // the gate never opens below this
constexpr float HYSTERESIS_DB = 3.0f;
constexpr float SILENCE_DB = -95.0f;  // below this is (near) digital silence
constexpr int FLOOR_HISTORY = 1000;   // 10 s of frame energies
constexpr float FLOOR_PERCENTILE = 0.1f;

constexpr int SAMPLES_PER_MS = SAMPLE_RATE / 1000;
constexpr int CONT_DECISION_FRAMES = CHUNK_STEP_MS / FRAME_MS;  // a decision every 50 ms
constexpr int CONT_START_DECISIONS = 3;  // won in a row before it starts
constexpr int CONT_STOP_DECISIONS = 4;   // lost in a row before it stops
constexpr int CONT_QUIET_STOP = 6;       // quiet frames before it stops

// How similar an action's samples typically are to each other, for an action
// with just one sample (with more, it's measured).
constexpr float DEFAULT_TYPICAL_SIMILARITY = 0.85f;

constexpr uint32_t NOT_A_COMMAND = 0;

// Runs f(i) for i in [0, n) on all cores.
template <typename F>
void parallel_for(size_t n, F f) {
    const size_t threads = std::min<size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
    if (threads <= 1) {
        for (size_t i = 0; i < n; i++) {
            f(i);
        }
        return;
    }
    std::atomic<size_t> next{ 0 };
    std::vector<std::thread> pool;
    for (size_t t = 0; t < threads; t++) {
        pool.emplace_back([&]() {
            for (size_t i; (i = next++) < n;) {
                f(i);
            }
        });
    }
    for (std::thread& t : pool) {
        t.join();
    }
}

// For a vector, the best similarity to each owner's vectors.
std::map<uint32_t, float> best_per_owner(const SoundVector& v, const std::vector<SoundVector>& vectors,
    const std::vector<uint32_t>& owners) {
    std::map<uint32_t, float> best;
    for (size_t i = 0; i < vectors.size(); i++) {
        const float sim = dot(v, vectors[i]);
        auto it = best.find(owners[i]);
        if (it == best.end() || sim > it->second) {
            best[owners[i]] = sim;
        }
    }
    return best;
}

// Adds what isn't any action to the actions' vectors. Negatives that are as
// close to an action's vectors as those are to each other are left out: the
// recordings win. If a common word (or the start of one of the user's words)
// sounds like a command, it's the command. `keep` is added as is.
void add_negatives(std::vector<SoundVector>& vectors, std::vector<uint32_t>& owners,
    const std::vector<SoundVector>& negatives, const std::vector<SoundVector>& keep) {
    const size_t commands = vectors.size();
    if (commands == 0) {
        return;
    }
    // How close each action's vectors typically are to each other: the median
    // over its vectors of the nearest other one.
    std::vector<float> nearest(commands, -2.0f);
    parallel_for(commands, [&](size_t j) {
        float best = -2.0f;
        for (size_t k = 0; k < commands; k++) {
            if (k != j && owners[k] == owners[j]) {
                best = std::max(best, dot(vectors[j], vectors[k]));
            }
        }
        nearest[j] = best;
    });
    std::map<uint32_t, std::vector<float>> per_action;
    for (size_t j = 0; j < commands; j++) {
        std::vector<float>& v = per_action[owners[j]];
        if (nearest[j] > -2.0f) {
            v.push_back(nearest[j]);
        }
    }
    std::map<uint32_t, float> typical;
    for (auto& [id, v] : per_action) {
        if (v.empty()) {
            typical[id] = DEFAULT_TYPICAL_SIMILARITY;
        } else {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            typical[id] = v[v.size() / 2];
        }
    }
    std::vector<float> limit(commands);
    for (size_t i = 0; i < commands; i++) {
        limit[i] = typical[owners[i]];
    }

    std::vector<char> conflicts(negatives.size(), 0);
    parallel_for(negatives.size(), [&](size_t n) {
        for (size_t i = 0; i < commands; i++) {
            if (dot(negatives[n], vectors[i]) >= limit[i]) {
                conflicts[n] = 1;
                return;
            }
        }
    });
    for (size_t n = 0; n < negatives.size(); n++) {
        if (!conflicts[n]) {
            vectors.push_back(negatives[n]);
            owners.push_back(NOT_A_COMMAND);
        }
    }
    for (const SoundVector& v : keep) {
        vectors.push_back(v);
        owners.push_back(NOT_A_COMMAND);
    }
}

}  // namespace

struct Recognizer::Cached {
    // Whole sounds: the sample's vectors, and those of the parts of it that
    // shouldn't count as the sound.
    std::vector<SoundVector> vectors;
    std::vector<SoundVector> confusable;
    // Continuous sounds: chunks of the sample, and of its confusable parts.
    std::vector<SoundVector> chunks;
    std::vector<SoundVector> confusable_chunks;
    bool has_vectors = false;
    bool has_confusable = false;
    bool has_chunks = false;
    bool has_confusable_chunks = false;
    std::wstring file;  // where they're kept between runs, if anywhere
};

namespace {

// Bump when the features change, so stale cached features aren't used.
constexpr uint32_t FEATURES_VERSION = 1;

uint64_t audio_hash(const Audio& audio) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    const uint8_t* p = (const uint8_t*) audio.data();
    for (size_t i = 0; i < audio.size() * sizeof(float); i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

std::wstring cache_file(const std::wstring& dir, uint64_t hash) {
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.vec", (unsigned long long) hash);
    return dir + L"\\" + name;
}

// File: "VHTV", version, then for each of the four vector sets: uint8 present,
// uint32 count, vectors.
bool load_features(const std::wstring& path, Recognizer::Cached& c) {
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    uint32_t version = 0;
    if (!f.read(magic, 4) || memcmp(magic, "VHTV", 4) != 0 || !f.read((char*) &version, 4) ||
        version != FEATURES_VERSION) {
        return false;
    }
    std::pair<bool*, std::vector<SoundVector>*> sets[] = { { &c.has_vectors, &c.vectors },
        { &c.has_confusable, &c.confusable }, { &c.has_chunks, &c.chunks },
        { &c.has_confusable_chunks, &c.confusable_chunks } };
    for (auto& [has, vecs] : sets) {
        uint8_t present = 0;
        uint32_t count = 0;
        if (!f.read((char*) &present, 1) || !f.read((char*) &count, 4) || count > 100000) {
            return false;
        }
        vecs->resize(count);
        if (count > 0 && !f.read((char*) vecs->data(), count * sizeof(SoundVector))) {
            return false;
        }
        *has = present != 0;
    }
    return true;
}

void save_features(const std::wstring& path, const Recognizer::Cached& c) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write("VHTV", 4);
    f.write((const char*) &FEATURES_VERSION, 4);
    std::pair<bool, const std::vector<SoundVector>*> sets[] = { { c.has_vectors, &c.vectors },
        { c.has_confusable, &c.confusable }, { c.has_chunks, &c.chunks },
        { c.has_confusable_chunks, &c.confusable_chunks } };
    for (auto& [has, vecs] : sets) {
        const uint8_t present = has ? 1 : 0;
        const uint32_t count = (uint32_t) vecs->size();
        f.write((const char*) &present, 1);
        f.write((const char*) &count, 4);
        f.write((const char*) vecs->data(), count * sizeof(SoundVector));
    }
}

}  // namespace

struct Recognizer::Model {
    struct Sound {
        uint32_t action_id = 0;
        SoundMode mode = SoundMode::Whole;
        float threshold = 0.0f;
        std::vector<std::shared_ptr<Cached>> samples;
        std::vector<uint64_t> sample_ids;
    };
    std::vector<Sound> sounds;
    std::vector<int> continuous;  // indices into sounds
    std::vector<int> whole;
    // What sounds are matched against: every vector, and whose it is
    // (NOT_A_COMMAND for the background, the confusable parts, ignored sounds
    // and, for continuous sounds, the whole sounds).
    std::vector<SoundVector> vectors;
    std::vector<uint32_t> owners;
    std::vector<SoundVector> chunk_vectors;
    std::vector<uint32_t> chunk_owners;

    const Sound* find(uint32_t action_id) const {
        for (const Sound& s : sounds) {
            if (s.action_id == action_id) {
                return &s;
            }
        }
        return nullptr;
    }
};

Recognizer::Recognizer() {
    builder_ = std::thread(&Recognizer::builder_main, this);
}

Recognizer::~Recognizer() {
    stop();
}

void Recognizer::stop() {
    {
        std::lock_guard<std::mutex> lock(build_mutex_);
        quit_ = true;
    }
    cancel_ = true;
    build_cv_.notify_all();
    if (builder_.joinable()) {
        builder_.join();
    }
}

void Recognizer::set_sounds(const std::vector<SoundSpec>& sounds) {
    {
        std::lock_guard<std::mutex> lock(build_mutex_);
        pending_specs_ = sounds;
        specs_pending_ = true;
        building_ = true;
    }
    build_cv_.notify_all();
}

void Recognizer::set_vad(float margin_db, int end_delay_ms) {
    margin_db_ = margin_db;
    end_delay_ms_ = end_delay_ms;
}

std::map<uint32_t, SoundInfo> Recognizer::sound_info() {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

bool Recognizer::building() {
    return building_.load();
}

void Recognizer::start_recording() {
    recording_ = true;
}

void Recognizer::stop_recording() {
    recording_ = false;
}

std::vector<RecordedClip> Recognizer::take_recorded() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecordedClip> out;
    out.swap(recorded_);
    return out;
}

RecognizerStatus Recognizer::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

// ---------------------------------------------------------------------------
// Model building

void Recognizer::builder_main() {
    while (true) {
        std::vector<SoundSpec> specs;
        {
            std::unique_lock<std::mutex> lock(build_mutex_);
            build_cv_.wait(lock, [this] { return quit_ || specs_pending_; });
            if (quit_) {
                return;
            }
            specs = std::move(pending_specs_);
            specs_pending_ = false;
        }
        build(specs);
        std::lock_guard<std::mutex> lock(build_mutex_);
        if (!specs_pending_) {
            building_ = false;
        }
    }
}


void Recognizer::build(const std::vector<SoundSpec>& specs) {
    auto model = std::make_shared<Model>();
    std::map<uint64_t, std::shared_ptr<Cached>> used;
    std::map<uint32_t, SoundInfo> info;
    const Background& bg = background();

    bool any_continuous = false;
    for (const SoundSpec& spec : specs) {
        any_continuous |= spec.mode == SoundMode::Continuous && !spec.samples.empty();
    }

    // Features of samples not seen before (the slow part: embeddings). Chunks
    // are only needed when there are continuous sounds: of their samples to
    // match, of all the others as what isn't them.
    struct Job {
        std::shared_ptr<Cached> cached;
        std::shared_ptr<const Audio> audio;
        bool vectors, confusable, chunks, confusable_chunks;
    };
    std::vector<Job> jobs;
    for (const SoundSpec& spec : specs) {
        for (const SampleData& sd : spec.samples) {
            if (!sd.audio || sd.audio->empty()) {
                continue;
            }
            auto it = cache_.find(sd.id);
            std::shared_ptr<Cached> c = it != cache_.end() ? it->second : nullptr;
            if (c == nullptr) {
                c = std::make_shared<Cached>();
                if (!cache_dir_.empty()) {
                    c->file = cache_file(cache_dir_, audio_hash(*sd.audio));
                    if (!load_features(c->file, *c)) {
                        *c = Cached();
                        c->file = cache_file(cache_dir_, audio_hash(*sd.audio));
                    }
                }
            }
            Job job{ c, sd.audio };
            job.vectors = spec.mode != SoundMode::Continuous && !c->has_vectors;
            job.confusable = spec.mode == SoundMode::Whole && !c->has_confusable;
            job.chunks = any_continuous && !c->has_chunks;
            job.confusable_chunks = any_continuous && spec.mode == SoundMode::Whole && !c->has_confusable_chunks;
            if (job.vectors || job.confusable || job.chunks || job.confusable_chunks) {
                c->has_vectors |= job.vectors;
                c->has_confusable |= job.confusable;
                c->has_chunks |= job.chunks;
                c->has_confusable_chunks |= job.confusable_chunks;
                jobs.push_back(job);
            }
            used[sd.id] = c;
        }
    }
    auto chunk_vectors = [&bg](const Audio& audio, std::vector<SoundVector>& out) {
        for (const Audio& chunk : sound_chunks(audio)) {
            for (const SoundVector& v : bg.vectors(describe_chunk(chunk))) {
                out.push_back(v);
            }
        }
    };
    parallel_for(jobs.size(), [&](size_t i) {
        if (cancel_) {
            return;
        }
        Job& job = jobs[i];
        if (job.vectors) {
            job.cached->vectors = bg.vectors(describe_sound(*job.audio));
        }
        if (job.chunks) {
            chunk_vectors(*job.audio, job.cached->chunks);
        }
        if (job.confusable || job.confusable_chunks) {
            const std::vector<Audio> parts = confusable_parts(*job.audio);
            for (size_t p = 0; p < parts.size(); p++) {
                if (job.confusable) {
                    for (const SoundVector& v : bg.vectors(describe_sound(parts[p]))) {
                        job.cached->confusable.push_back(v);
                    }
                }
                // The start of a sound is already among its own chunks.
                if (job.confusable_chunks && p > 0) {
                    chunk_vectors(parts[p], job.cached->confusable_chunks);
                }
            }
        }
    });

    if (cancel_) {
        return;  // quitting: the features are incomplete, don't keep them
    }
    for (const Job& job : jobs) {
        if (!job.cached->file.empty()) {
            save_features(job.cached->file, *job.cached);
        }
    }

    std::vector<SoundVector> negatives, chunk_negatives;  // pruned
    std::vector<SoundVector> ignored, ignored_chunks;     // never pruned: the user said so
    for (const SoundSpec& spec : specs) {
        Model::Sound s;
        s.action_id = spec.action_id;
        s.mode = spec.mode;
        s.threshold = spec.threshold;
        for (const SampleData& sd : spec.samples) {
            auto it = used.find(sd.id);
            if (it != used.end()) {
                s.samples.push_back(it->second);
                s.sample_ids.push_back(sd.id);
            }
        }
        SoundInfo si;
        si.samples = (int) s.samples.size();
        info[s.action_id] = si;
        if (s.samples.empty()) {
            continue;
        }
        for (const auto& c : s.samples) {
            switch (s.mode) {
                case SoundMode::Ignore:
                    ignored.insert(ignored.end(), c->vectors.begin(), c->vectors.end());
                    ignored_chunks.insert(ignored_chunks.end(), c->chunks.begin(), c->chunks.end());
                    break;
                case SoundMode::Whole:
                    for (const SoundVector& v : c->vectors) {
                        model->vectors.push_back(v);
                        model->owners.push_back(s.action_id);
                    }
                    negatives.insert(negatives.end(), c->confusable.begin(), c->confusable.end());
                    chunk_negatives.insert(chunk_negatives.end(), c->chunks.begin(), c->chunks.end());
                    chunk_negatives.insert(chunk_negatives.end(), c->confusable_chunks.begin(), c->confusable_chunks.end());
                    break;
                case SoundMode::Continuous:
                    for (const SoundVector& v : c->chunks) {
                        model->chunk_vectors.push_back(v);
                        model->chunk_owners.push_back(s.action_id);
                    }
                    break;
            }
        }
        if (s.mode != SoundMode::Ignore) {
            model->sounds.push_back(std::move(s));
        }
    }
    negatives.insert(negatives.end(), bg.negatives().begin(), bg.negatives().end());
    add_negatives(model->vectors, model->owners, negatives, ignored);
    chunk_negatives.insert(chunk_negatives.end(), bg.chunk_negatives().begin(), bg.chunk_negatives().end());
    add_negatives(model->chunk_vectors, model->chunk_owners, chunk_negatives, ignored_chunks);

    std::vector<Model::Sound*> whole, continuous;
    for (size_t i = 0; i < model->sounds.size(); i++) {
        Model::Sound& s = model->sounds[i];
        (s.mode == SoundMode::Continuous ? model->continuous : model->whole).push_back((int) i);
        (s.mode == SoundMode::Continuous ? continuous : whole).push_back(&s);
    }
    // Which other action each action's samples are closest to, leaving the
    // sample itself out. A sample counts as confused when most of its vectors
    // are closer to another action's samples than to its own action's other
    // samples.
    auto find_confusions = [&info](const std::vector<Model::Sound*>& sounds, std::vector<SoundVector> Cached::*field) {
        for (const auto* a : sounds) {
            std::map<uint32_t, std::vector<uint64_t>> confused;  // other action -> samples
            for (size_t si = 0; si < a->samples.size(); si++) {
                const auto& sample = a->samples[si];
                const std::vector<SoundVector>& mine = (*sample).*field;
                std::map<uint32_t, int> votes;
                for (const SoundVector& v : mine) {
                    float own = -1.0f, other = -1.0f;
                    uint32_t other_id = 0;
                    for (const auto* b : sounds) {
                        for (const auto& t : b->samples) {
                            if (t == sample) {
                                continue;
                            }
                            for (const SoundVector& w : (*t).*field) {
                                const float sim = dot(v, w);
                                if (b->action_id == a->action_id) {
                                    own = std::max(own, sim);
                                } else if (sim > other) {
                                    other = sim;
                                    other_id = b->action_id;
                                }
                            }
                        }
                    }
                    if (other_id != 0 && other > own) {
                        votes[other_id]++;
                    }
                }
                for (const auto& [id, n] : votes) {
                    if (2 * n > (int) mine.size() || (mine.size() == 1 && n == 1)) {
                        confused[id].push_back(a->sample_ids[si]);
                    }
                }
            }
            SoundInfo& info_a = info[a->action_id];
            for (const auto& [id, ids] : confused) {
                if ((int) ids.size() > info_a.confused_samples) {
                    info_a.confused_samples = (int) ids.size();
                    info_a.confused_sample_ids = ids;
                    info_a.similar_action = id;
                }
            }
        }
};
    find_confusions(whole, &Cached::vectors);
    find_confusions(continuous, &Cached::chunks);

    cache_ = std::move(used);  // forgets samples that are gone
    if (!cache_dir_.empty()) {
        // And their features kept on disk.
        std::set<std::wstring> keep;
        for (const auto& [id, c] : cache_) {
            keep.insert(c->file.substr(c->file.find_last_of(L'\\') + 1));
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((cache_dir_ + L"\\*.vec").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!keep.count(fd.cFileName)) {
                    DeleteFileW((cache_dir_ + L"\\" + fd.cFileName).c_str());
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    model_ = model;
    info_ = std::move(info);
}

// ---------------------------------------------------------------------------
// Streaming

void Recognizer::emit(uint32_t action_id, TriggerType type, float score) {
    if (callback_) {
        Detection d;
        d.action_id = action_id;
        d.type = type;
        d.score = score;
        callback_(d);
    }
}

void Recognizer::reset_stream() {
    stop_continuous();
    frontend_.reset();
    frame_index_ = 0;
    signal_seen_ = false;
    samples_seen_ = 0;
    audio_.clear();
    audio_start_ = 0;
    preroll_.clear();
    in_segment_ = false;
    seg_frames_.clear();
}

void Recognizer::process(const float* samples, size_t n) {
    std::shared_ptr<const Model> model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        model = model_;
    }
    if (!model) {
        static const Model empty;
        model = std::shared_ptr<const Model>(&empty, [](const Model*) {});
    }
    if (model != stream_model_) {
        // New templates: whatever continuous sound was on is over.
        stop_continuous();
        stream_model_ = model;
        cont_candidate_ = 0;
        cont_count_ = cont_lost_ = cont_quiet_ = 0;
        scores_.clear();
    }

    const bool recording = recording_.load();
    if (recording != was_recording_) {
        // Don't let a sound straddle recording and matching.
        was_recording_ = recording;
        stop_continuous();
        in_segment_ = false;
        seg_frames_.clear();
    }
    if (audio_.empty()) {
        audio_start_ = samples_seen_;
    }
    if (audio_.size() < MAX_AUDIO) {
        audio_.insert(audio_.end(), samples, samples + std::min(n, MAX_AUDIO - audio_.size()));
    }
    samples_seen_ += n;

    new_frames_.clear();
    frontend_.process(samples, n, new_frames_);
    for (const Frame& f : new_frames_) {
        on_frame(f, *model);
        frame_index_++;
    }

    if (!in_segment_) {
        // Keep just enough audio for the pre-roll of the next sound.
        const uint64_t keep_from = (frame_index_ > PREROLL_FRAMES ? frame_index_ - PREROLL_FRAMES : 0) * HOP;
        if (keep_from > audio_start_) {
            const size_t drop = (size_t) std::min<uint64_t>(keep_from - audio_start_, audio_.size());
            audio_.erase(audio_.begin(), audio_.begin() + drop);
            audio_start_ += drop;
        }
    }
}

void Recognizer::on_frame(const Frame& f, const Model& model) {
    // Background level: a low percentile of the last 10 s of frame energies.
    if (floor_history_.size() < FLOOR_HISTORY) {
        floor_history_.push_back(f.energy_db);
    } else {
        floor_history_[floor_pos_] = f.energy_db;
        floor_pos_ = (floor_pos_ + 1) % FLOOR_HISTORY;
    }
    if (!floor_init_ || frame_index_ % 25 == 0) {
        floor_sorted_ = floor_history_;
        const size_t k = (size_t) (floor_sorted_.size() * FLOOR_PERCENTILE);
        std::nth_element(floor_sorted_.begin(), floor_sorted_.begin() + k, floor_sorted_.end());
        floor_db_ = floor_sorted_[k];
        floor_init_ = true;
    }
    const float threshold = std::max(floor_db_ + margin_db_.load(), ABS_MIN_DB);
    const bool recording = recording_.load();
    const bool loud = f.energy_db > (in_segment_ ? threshold - HYSTERESIS_DB : threshold);

    if (!in_segment_) {
        if (loud) {
            in_segment_ = true;
            seg_start_ = frame_index_ - preroll_.size();
            seg_frames_ = preroll_;
            seg_frames_.push_back(f);
            first_loud_ = last_loud_ = frame_index_;
            seg_continuous_ = false;
            // Scores shown are for the sound being heard now.
            for (const Model::Sound& s : model.sounds) {
                scores_[s.action_id] = -1.0f;
            }
            cont_candidate_ = 0;
            cont_count_ = cont_lost_ = cont_quiet_ = 0;
        } else {
            preroll_.push_back(f);
            if (preroll_.size() > PREROLL_FRAMES) {
                preroll_.erase(preroll_.begin());
            }
        }
    } else {
        if (seg_frames_.size() < (recording ? MAX_RECORD_FRAMES : MAX_SEG_FRAMES)) {
            seg_frames_.push_back(f);
        }
        if (loud) {
            last_loud_ = frame_index_;
        }
    }

    if (in_segment_ && !recording) {
        continuous_frame(loud, model);
    }

    if (in_segment_) {
        const int hangover = std::max(1, end_delay_ms_.load() / FRAME_MS);
        const bool too_long = recording && frame_index_ - seg_start_ >= MAX_RECORD_FRAMES;
        if ((int) (frame_index_ - last_loud_) >= hangover || too_long) {
            end_segment(model);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (f.energy_db > SILENCE_DB) {
        signal_seen_ = true;
    }
    status_.signal_seen = signal_seen_;
    status_.stream_seconds = frame_index_ * FRAME_MS / 1000.0f;
    status_.level_db = f.energy_db;
    status_.floor_db = floor_db_;
    status_.threshold_db = threshold;
    status_.in_sound = in_segment_;
    status_.recording = recording;
    status_.scores = scores_;
    status_.active_continuous = cont_active_;
}

void Recognizer::continuous_frame(bool loud, const Model& model) {
    if (model.continuous.empty() || model.chunk_vectors.empty()) {
        return;
    }
    if (!loud) {
        cont_quiet_++;
        if (cont_quiet_ >= CONT_QUIET_STOP) {
            stop_continuous();
        }
        return;
    }
    cont_quiet_ = 0;
    if ((frame_index_ + 1 - seg_start_) % CONT_DECISION_FRAMES != 0) {
        return;
    }

    // What has been heard of the sound so far, or its last 500 ms.
    const uint64_t end = std::min<uint64_t>(samples_seen_, frame_index_ * HOP + WINDOW);
    uint64_t start = std::max<uint64_t>(seg_start_ * HOP, audio_start_);
    if (end <= start || end - audio_start_ > audio_.size()) {
        return;
    }
    if (end - start > (uint64_t) MAX_PREFIX_MS * SAMPLES_PER_MS) {
        start = end - (uint64_t) SLIDING_CHUNK_MS * SAMPLES_PER_MS;
    }
    const Audio chunk(audio_.begin() + (size_t) (start - audio_start_), audio_.begin() + (size_t) (end - audio_start_));
    const std::vector<SoundVector> heard = background().vectors(describe_chunk(chunk));
    if (heard.empty()) {
        return;
    }

    // The winner, and how much closer it is than anything else.
    const std::map<uint32_t, float> best = best_per_owner(heard[0], model.chunk_vectors, model.chunk_owners);
    uint32_t winner = NOT_A_COMMAND;
    float first = -2.0f, second = -2.0f;
    for (const auto& [id, sim] : best) {
        if (sim > first) {
            second = first;
            first = sim;
            winner = id;
        } else if (sim > second) {
            second = sim;
        }
    }
    for (int idx : model.continuous) {
        const uint32_t id = model.sounds[idx].action_id;
        auto it = best.find(id);
        if (it != best.end()) {
            scores_[id] = id == winner ? first - second : it->second - first;
        }
    }
    const float margin = first - second;

    if (cont_active_ == 0) {
        const Model::Sound* s = winner != NOT_A_COMMAND ? model.find(winner) : nullptr;
        if (s != nullptr && s->mode == SoundMode::Continuous && margin >= s->threshold) {
            cont_count_ = winner == cont_candidate_ ? cont_count_ + 1 : 1;
            cont_candidate_ = winner;
            if (cont_count_ >= CONT_START_DECISIONS) {
                cont_active_ = winner;
                cont_lost_ = 0;
                seg_continuous_ = true;
                emit(winner, TriggerType::Start, margin);
            }
        } else {
            cont_candidate_ = 0;
            cont_count_ = 0;
        }
    } else {
        cont_lost_ = winner == cont_active_ ? 0 : cont_lost_ + 1;
        if (cont_lost_ >= CONT_STOP_DECISIONS) {
            stop_continuous();
        }
    }
}

void Recognizer::stop_continuous() {
    if (cont_active_ == 0) {
        return;
    }
    const uint32_t id = cont_active_;
    cont_active_ = 0;
    cont_candidate_ = 0;
    cont_count_ = 0;
    emit(id, TriggerType::Stop, 0.0f);
}

void Recognizer::match_whole(const Audio& audio, const Model& model) {
    const std::vector<SoundVector> heard = background().vectors(describe_sound(audio));
    // For each action, the best margin over the sound's windows: how much
    // closer the window is to that action's samples than to anything else.
    std::map<uint32_t, float> margin;
    for (const SoundVector& v : heard) {
        const std::map<uint32_t, float> best = best_per_owner(v, model.vectors, model.owners);
        for (const auto& [id, sim] : best) {
            if (id == NOT_A_COMMAND) {
                continue;
            }
            float other = -1.0f;
            for (const auto& [id2, sim2] : best) {
                if (id2 != id) {
                    other = std::max(other, sim2);
                }
            }
            auto it = margin.find(id);
            if (it == margin.end() || sim - other > it->second) {
                margin[id] = sim - other;
            }
        }
    }
    uint32_t best_id = 0;
    float best_margin = -1e9f;
    for (int idx : model.whole) {
        const Model::Sound& s = model.sounds[idx];
        auto it = margin.find(s.action_id);
        if (it == margin.end()) {
            continue;
        }
        scores_[s.action_id] = it->second;
        if (it->second > best_margin) {
            best_margin = it->second;
            best_id = s.action_id;
        }
    }
    const Model::Sound* s = best_id != 0 ? model.find(best_id) : nullptr;
    if (s != nullptr && best_margin >= s->threshold) {
        emit(best_id, TriggerType::Fire, best_margin);
    }
}

void Recognizer::end_segment(const Model& model) {
    in_segment_ = false;
    preroll_.clear();
    stop_continuous();
    // Drop the quiet tail.
    const size_t keep = (size_t) (last_loud_ + TAIL_FRAMES + 1 - seg_start_);
    if (seg_frames_.size() > keep) {
        seg_frames_.resize(keep);
    }

    // The sound's audio, cut the same way for recording and matching. A sound
    // right at the start of recording has no pre-roll audio.
    Audio clip;
    const uint64_t start = std::max<uint64_t>(seg_start_ * HOP, audio_start_);
    const uint64_t end = std::min<uint64_t>(samples_seen_, (last_loud_ + TAIL_FRAMES) * HOP + WINDOW);
    if (end > start && end - audio_start_ <= audio_.size()) {
        clip.assign(audio_.begin() + (size_t) (start - audio_start_), audio_.begin() + (size_t) (end - audio_start_));
    }

    if (recording_.load()) {
        if (!clip.empty()) {
            RecordedClip rc;
            rc.audio = std::move(clip);
            rc.loud_start_ms = first_loud_ * FRAME_MS;
            rc.loud_end_ms = (last_loud_ + 1) * FRAME_MS;
            std::lock_guard<std::mutex> lock(mutex_);
            recorded_.push_back(std::move(rc));
            status_.sounds_heard++;
        }
        seg_frames_.clear();
        return;
    }

    const size_t n = seg_frames_.size();
    const bool whole_candidate = !seg_continuous_ && n >= MIN_WHOLE_FRAMES && n <= MAX_WHOLE_FRAMES &&
                                 (last_loud_ - seg_start_) < MAX_SEG_FRAMES;
    if (whole_candidate && !model.whole.empty() && !clip.empty()) {
        match_whole(clip, model);
    }
    seg_frames_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.sounds_heard++;
}
