// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <vector>

// Google's speech embedding model (speech_embedding on TFHub, Apache-2.0, in
// the form openWakeWord uses), run on the CPU. It turns 76 frames (~0.8 s) of
// a mel spectrogram into a 96-dimensional vector describing the sound. It was
// trained on lots of speakers and recording conditions, so the same word gives
// similar vectors regardless of the microphone's position or the room.
namespace speech {

constexpr int MEL_BINS = 32;
constexpr int EMBED_FRAMES = 76;
constexpr int EMBED_DIM = 96;

using Embedding = std::array<float, EMBED_DIM>;

class Embedder {
public:
    // The contents of models/speech_embedding.bin.
    bool load(const void* data, size_t size);
    bool loaded() const { return !layers_.empty(); }

    // Mel spectrogram the way the model expects it: frames x MEL_BINS, one
    // frame every 10 ms. Audio is 16 kHz, -1..1.
    std::vector<float> mel(const float* audio, size_t n) const;

    // One embedding from EMBED_FRAMES mel frames.
    Embedding embed(const float* mel) const;

    // Embeddings of a clip: its loud part centered in one window, or for
    // longer sounds, windows every 80 ms.
    std::vector<Embedding> embed_clip(const std::vector<float>& audio) const;

    // One embedding with the end of the audio at the end of the window (the
    // newest audio last, as when listening), quiet before it if it's shorter.
    Embedding embed_tail(const std::vector<float>& audio) const;

private:
    struct Layer {
        int out = 0, in = 0, kh = 0, kw = 0;
        std::vector<float> weights;  // kh x kw x in x out (output channels innermost)
        std::vector<float> bias;     // empty if none
    };
    std::vector<Layer> layers_;
};

// The model built into the executable (empty if it couldn't be loaded).
const Embedder& embedder();

// The loud part of a clip: frames within 35 dB of the loudest, plus 30 ms on
// each side.
std::vector<float> trim_to_loud(const std::vector<float>& audio);

}  // namespace speech
