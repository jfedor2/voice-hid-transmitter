// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "embedding.h"
#include "features.h"

// Whole sounds are matched as fixed-length vectors that combine
//  - the speech embedding (what was said, robust to microphone and room), and
//  - statistics of the cepstra of the sound's loud frames (its spectral
//    shape, which tells apart sounds the speech model doesn't know well,
//    like different tongue clicks).
// Both halves are normalized against a background of common words and noises,
// then the whole vector is normalized, so matching is by cosine similarity.
// A sound longer than the embedding window gives several vectors.

constexpr int STATS_DIM = NUM_CEPS * 2;  // mean and standard deviation
constexpr int VECTOR_DIM = speech::EMBED_DIM + STATS_DIM;

using SoundVector = std::array<float, VECTOR_DIM>;
using Stats = std::array<float, STATS_DIM>;

// What a sound is made of, before normalization.
struct SoundDescription {
    std::vector<speech::Embedding> windows;
    Stats stats{};
};

SoundDescription describe_sound(const std::vector<float>& audio);

// Continuous sounds are matched while they last, from chunks of them: the
// first 50, 100, ... 700 ms as the sound starts, and the last 500 ms after
// that. A chunk is described like a whole sound, but with its end at the end
// of the embedding window.
constexpr int CHUNK_STEP_MS = 50;
constexpr int MAX_PREFIX_MS = 700;
constexpr int SLIDING_CHUNK_MS = 500;

SoundDescription describe_chunk(const std::vector<float>& audio);

// The chunks of a recorded sound, as they'd be seen while listening to it
// (prefixes up to max_prefix_ms, then sliding chunks every 200 ms).
std::vector<std::vector<float>> sound_chunks(const std::vector<float>& audio, int max_prefix_ms = MAX_PREFIX_MS);

float dot(const SoundVector& a, const SoundVector& b);

// The background: normalization statistics, and built-in examples of things
// that aren't commands (common words, breaths, clicks, bumps).
class Background {
public:
    Background() { stats_scale_.fill(1.0f); }

    bool load(const void* data, size_t size);
    bool loaded() const { return loaded_; }

    std::vector<SoundVector> vectors(const SoundDescription& d) const;
    // Whole sounds that aren't commands, and chunks of them (for continuous sounds).
    const std::vector<SoundVector>& negatives() const { return negatives_; }
    const std::vector<SoundVector>& chunk_negatives() const { return chunk_negatives_; }

    // Builds the background file from sounds that aren't commands, whole and in chunks.
    static std::vector<uint8_t> build(const std::vector<SoundDescription>& sounds,
        const std::vector<SoundDescription>& chunks);

private:
    bool loaded_ = false;
    speech::Embedding embedding_mean_{};
    Stats stats_mean_{};
    Stats stats_scale_{};
    std::vector<SoundVector> negatives_;
    std::vector<SoundVector> chunk_negatives_;
};

// The background built into the executable.
const Background& background();

// Things that aren't the recorded sound but are easily mistaken for it: its
// beginning and its end (like the "ft" of "left"), and for longer sounds,
// the sound backwards.
std::vector<std::vector<float>> confusable_parts(const std::vector<float>& audio);

// Noises that aren't commands, for the background: breaths, bursts, clicks, bumps.
std::vector<std::vector<float>> synthetic_noises();
