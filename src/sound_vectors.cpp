// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "sound_vectors.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace {

constexpr float LOUD_FRAMES_DB = 20.0f;  // frames this far below the loudest count for the statistics
constexpr float PARTS_LOUD_DB = 30.0f;   // what counts as the loud part when cutting a sound up
constexpr size_t MIN_PARTS_SAMPLES = SAMPLE_RATE / 5;      // shorter sounds aren't cut up
constexpr size_t MIN_REVERSE_SAMPLES = SAMPLE_RATE / 4;    // shorter sounds sound the same backwards
constexpr float PART_FRACTION = 0.35f;
constexpr float CHUNK_LOUD_DB = 35.0f;   // what counts as the sound when cutting it into chunks

template <size_t N>
void normalize(float* v) {
    double sum = 0;
    for (size_t i = 0; i < N; i++) {
        sum += (double) v[i] * v[i];
    }
    const float scale = sum > 0 ? (float) (1.0 / std::sqrt(sum)) : 0.0f;
    for (size_t i = 0; i < N; i++) {
        v[i] *= scale;
    }
}

// Start and end (in samples) of the frames within `range_db` of the loudest.
bool loud_span(const std::vector<float>& audio, float range_db, size_t& start, size_t& end) {
    const size_t frames = audio.size() / HOP;
    if (frames == 0) {
        return false;
    }
    std::vector<float> db(frames);
    float top = -1e30f;
    for (size_t f = 0; f < frames; f++) {
        double sum = 0;
        for (int i = 0; i < HOP; i++) {
            sum += (double) audio[f * HOP + i] * audio[f * HOP + i];
        }
        db[f] = (float) (10.0 * std::log10(sum / HOP + 1e-10));
        top = std::max(top, db[f]);
    }
    size_t first = frames, last = 0;
    for (size_t f = 0; f < frames; f++) {
        if (db[f] > top - range_db) {
            first = std::min(first, f);
            last = f;
        }
    }
    start = first * HOP;
    end = (last + 1) * HOP;
    return first < frames;
}

}  // namespace

float dot(const SoundVector& a, const SoundVector& b) {
    float s = 0;
    for (int i = 0; i < VECTOR_DIM; i++) {
        s += a[i] * b[i];
    }
    return s;
}

// Statistics of the cepstra of the loud frames.
static Stats loud_stats(const std::vector<float>& audio) {
    Stats stats{};
    std::vector<float> loud = speech::trim_to_loud(audio);
    if (loud.size() < WINDOW) {
        loud.resize(WINDOW, 0.0f);
    }
    const std::vector<Frame> frames = FeatureFrontend::frames_of(loud);
    float top = -1e30f;
    for (const Frame& f : frames) {
        top = std::max(top, f.energy_db);
    }
    double sum[NUM_CEPS] = {}, sum_sq[NUM_CEPS] = {};
    int n = 0;
    for (const Frame& f : frames) {
        if (f.energy_db < top - LOUD_FRAMES_DB) {
            continue;
        }
        n++;
        for (int k = 0; k < NUM_CEPS; k++) {
            sum[k] += f.ceps[k];
            sum_sq[k] += (double) f.ceps[k] * f.ceps[k];
        }
    }
    for (int k = 0; k < NUM_CEPS && n > 0; k++) {
        const double mean = sum[k] / n;
        stats[k] = (float) mean;
        stats[NUM_CEPS + k] = (float) std::sqrt(std::max(0.0, sum_sq[k] / n - mean * mean));
    }
    return stats;
}

SoundDescription describe_sound(const std::vector<float>& audio) {
    SoundDescription d;
    d.windows = speech::embedder().embed_clip(audio);
    d.stats = loud_stats(audio);
    return d;
}

SoundDescription describe_chunk(const std::vector<float>& audio) {
    SoundDescription d;
    d.windows.push_back(speech::embedder().embed_tail(audio));
    d.stats = loud_stats(audio);
    return d;
}

std::vector<std::vector<float>> sound_chunks(const std::vector<float>& audio, int max_prefix_ms) {
    std::vector<std::vector<float>> out;
    size_t s0, s1;
    if (!loud_span(audio, CHUNK_LOUD_DB, s0, s1)) {
        return out;
    }
    s0 = s0 >= 3 * HOP ? s0 - 3 * HOP : 0;  // like the pre-roll when listening
    const size_t len = s1 - s0;
    const size_t per_ms = SAMPLE_RATE / 1000;
    for (int ms = CHUNK_STEP_MS; ms <= max_prefix_ms; ms += CHUNK_STEP_MS) {
        const size_t n = ms * per_ms;
        if (n > len + 50 * per_ms) {
            break;
        }
        out.emplace_back(audio.begin() + s0, audio.begin() + std::min(audio.size(), s0 + n));
    }
    const size_t window = SLIDING_CHUNK_MS * per_ms;
    for (size_t end = s0 + window; end + window / 2 <= s1; end += 200 * per_ms) {
        out.emplace_back(audio.begin() + (end - window), audio.begin() + end);
    }
    return out;
}

std::vector<SoundVector> Background::vectors(const SoundDescription& d) const {
    std::vector<SoundVector> out;
    float stats[STATS_DIM];
    for (int i = 0; i < STATS_DIM; i++) {
        stats[i] = (d.stats[i] - stats_mean_[i]) / stats_scale_[i];
    }
    normalize<STATS_DIM>(stats);
    for (const speech::Embedding& e : d.windows) {
        SoundVector v;
        for (int i = 0; i < speech::EMBED_DIM; i++) {
            v[i] = e[i] - embedding_mean_[i];
        }
        normalize<speech::EMBED_DIM>(v.data());
        std::copy(stats, stats + STATS_DIM, v.begin() + speech::EMBED_DIM);
        normalize<VECTOR_DIM>(v.data());
        out.push_back(v);
    }
    return out;
}

// File: "VHTB", uint32 version, embedding mean, stats mean, stats scale,
// uint32 count, count vectors (whole sounds), uint32 count, count vectors (chunks).
std::vector<uint8_t> Background::build(const std::vector<SoundDescription>& sounds,
    const std::vector<SoundDescription>& chunks) {
    Background bg;
    double emb_sum[speech::EMBED_DIM] = {};
    size_t windows = 0;
    double st_sum[STATS_DIM] = {}, st_sq[STATS_DIM] = {};
    for (const SoundDescription& d : sounds) {
        for (const speech::Embedding& e : d.windows) {
            for (int i = 0; i < speech::EMBED_DIM; i++) {
                emb_sum[i] += e[i];
            }
            windows++;
        }
        for (int i = 0; i < STATS_DIM; i++) {
            st_sum[i] += d.stats[i];
            st_sq[i] += (double) d.stats[i] * d.stats[i];
        }
    }
    for (int i = 0; i < speech::EMBED_DIM; i++) {
        bg.embedding_mean_[i] = (float) (emb_sum[i] / std::max<size_t>(windows, 1));
    }
    const double n = (double) std::max<size_t>(sounds.size(), 1);
    for (int i = 0; i < STATS_DIM; i++) {
        const double mean = st_sum[i] / n;
        bg.stats_mean_[i] = (float) mean;
        bg.stats_scale_[i] = (float) std::sqrt(std::max(0.0, st_sq[i] / n - mean * mean)) + 1e-6f;
    }
    std::vector<SoundVector> vecs, chunk_vecs;
    for (const SoundDescription& d : sounds) {
        for (const SoundVector& v : bg.vectors(d)) {
            vecs.push_back(v);
        }
    }
    for (const SoundDescription& d : chunks) {
        for (const SoundVector& v : bg.vectors(d)) {
            chunk_vecs.push_back(v);
        }
    }

    std::vector<uint8_t> out;
    auto put = [&out](const void* p, size_t n) {
        out.insert(out.end(), (const uint8_t*) p, (const uint8_t*) p + n);
    };
    const uint32_t version = 2, count = (uint32_t) vecs.size(), chunk_count = (uint32_t) chunk_vecs.size();
    put("VHTB", 4);
    put(&version, 4);
    put(bg.embedding_mean_.data(), sizeof(bg.embedding_mean_));
    put(bg.stats_mean_.data(), sizeof(bg.stats_mean_));
    put(bg.stats_scale_.data(), sizeof(bg.stats_scale_));
    put(&count, 4);
    for (const SoundVector& v : vecs) {
        put(v.data(), sizeof(v));
    }
    put(&chunk_count, 4);
    for (const SoundVector& v : chunk_vecs) {
        put(v.data(), sizeof(v));
    }
    return out;
}

bool Background::load(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*) data;
    const uint8_t* end = p + size;
    auto get = [&](void* dst, size_t n) {
        if ((size_t) (end - p) < n) {
            return false;
        }
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    char magic[4];
    uint32_t version = 0, count = 0;
    if (!get(magic, 4) || memcmp(magic, "VHTB", 4) != 0 || !get(&version, 4) || version != 2 ||
        !get(embedding_mean_.data(), sizeof(embedding_mean_)) || !get(stats_mean_.data(), sizeof(stats_mean_)) ||
        !get(stats_scale_.data(), sizeof(stats_scale_)) || !get(&count, 4)) {
        return false;
    }
    negatives_.resize(count);
    for (SoundVector& v : negatives_) {
        if (!get(v.data(), sizeof(v))) {
            negatives_.clear();
            return false;
        }
    }
    if (!get(&count, 4)) {
        return false;
    }
    chunk_negatives_.resize(count);
    for (SoundVector& v : chunk_negatives_) {
        if (!get(v.data(), sizeof(v))) {
            chunk_negatives_.clear();
            return false;
        }
    }
    loaded_ = true;
    return true;
}

const Background& background() {
    // Never destroyed: threads may still be using it while the program exits.
    static const Background& bg = *[]() {
        Background* b = new Background();
        HMODULE module = GetModuleHandleW(nullptr);
        if (HRSRC res = FindResourceW(module, L"SOUND_BACKGROUND", RT_RCDATA)) {
            if (HGLOBAL h = LoadResource(module, res)) {
                b->load(LockResource(h), SizeofResource(module, res));
            }
        }
        return b;
    }();
    return bg;
}

std::vector<std::vector<float>> confusable_parts(const std::vector<float>& audio) {
    std::vector<std::vector<float>> out;
    size_t s0, s1;
    if (!loud_span(audio, PARTS_LOUD_DB, s0, s1)) {
        return out;
    }
    const size_t len = s1 - s0;
    if (len > MIN_PARTS_SAMPLES) {
        const size_t part = (size_t) (len * PART_FRACTION);
        out.emplace_back(audio.begin() + s0, audio.begin() + s0 + part);
        out.emplace_back(audio.begin() + s1 - part, audio.begin() + s1);
    }
    if (len >= MIN_REVERSE_SAMPLES) {
        out.emplace_back(audio.rbegin(), audio.rend());
    }
    return out;
}

std::vector<std::vector<float>> synthetic_noises() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    auto envelope = [](size_t i, float attack, float decay) {
        const float t = (float) i / SAMPLE_RATE;
        return std::min(1.0f, t / attack) * std::exp(-t / decay);
    };
    std::vector<std::vector<float>> out;
    for (int k = 0; k < 40; k++) {
        std::vector<float> x;
        switch (k % 4) {
            case 0: {  // breath: low-passed noise
                x.resize((size_t) (SAMPLE_RATE * (0.15f + 0.45f * uni(rng))));
                const int smooth = 4 + (int) (uni(rng) * 16);
                std::vector<float> n(x.size());
                for (size_t i = 0; i < n.size(); i++) {
                    n[i] = gauss(rng) * envelope(i, 0.05f, 0.15f);
                }
                for (size_t i = 0; i < x.size(); i++) {
                    float s = 0;
                    for (int j = -smooth / 2; j < smooth - smooth / 2; j++) {
                        const long long p = (long long) i + j;
                        if (p >= 0 && p < (long long) n.size()) {
                            s += n[(size_t) p];
                        }
                    }
                    x[i] = s / smooth;
                }
                break;
            }
            case 1:  // burst
                x.resize((size_t) (SAMPLE_RATE * (0.02f + 0.13f * uni(rng))));
                for (size_t i = 0; i < x.size(); i++) {
                    x[i] = gauss(rng) * envelope(i, 0.002f, 0.03f);
                }
                break;
            case 2: {  // keyboard-like click
                const float decay = 0.002f + 0.008f * uni(rng);
                x.resize(SAMPLE_RATE / 20);
                for (size_t i = 0; i < x.size(); i++) {
                    x[i] = gauss(rng) * envelope(i, 0.0005f, decay);
                }
                break;
            }
            default: {  // bump
                const float hz = 50.0f + 150.0f * uni(rng);
                x.resize((size_t) (SAMPLE_RATE * 0.3f));
                for (size_t i = 0; i < x.size(); i++) {
                    x[i] = std::sin(2.0f * 3.14159265f * hz * i / SAMPLE_RATE) * envelope(i, 0.003f, 0.06f);
                }
                break;
            }
        }
        float peak = 1e-9f;
        for (float v : x) {
            peak = std::max(peak, std::fabs(v));
        }
        const float gain = std::pow(10.0f, (-24.0f + 18.0f * uni(rng)) / 20.0f) / peak;
        std::vector<float> padded(SAMPLE_RATE / 10, 0.0f);
        for (float v : x) {
            padded.push_back(v * gain);
        }
        padded.resize(padded.size() + SAMPLE_RATE / 10, 0.0f);
        out.push_back(std::move(padded));
    }
    return out;
}
