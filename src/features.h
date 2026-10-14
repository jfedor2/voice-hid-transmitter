// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <vector>

// Audio is 16 kHz mono float. The frontend turns it into one frame every
// 10 ms (25 ms window): the frame's energy and its mel-frequency cepstral
// coefficients c1..c12 (c0, the loudness, is left out so matching doesn't
// depend on how loud the sound is).
constexpr int SAMPLE_RATE = 16000;
constexpr int HOP = 160;
constexpr int WINDOW = 400;
constexpr int NUM_CEPS = 12;
constexpr int FRAME_MS = 1000 * HOP / SAMPLE_RATE;

struct Frame {
    float energy_db = -100.0f;  // dBFS
    float ceps[NUM_CEPS] = {};
};

class FeatureFrontend {
public:
    FeatureFrontend();

    // Feeds samples, appending a frame to `out` for every hop completed.
    void process(const float* samples, size_t n, std::vector<Frame>& out);
    void reset();

    // All frames of a recorded clip.
    static std::vector<Frame> frames_of(const std::vector<float>& audio);

private:
    void compute_frame(Frame& out);

    std::vector<float> buffer_;  // last WINDOW samples (pre-emphasized)
    size_t filled_ = 0;
    float prev_sample_ = 0.0f;
};

// Euclidean distance between two frames' cepstra.
float ceps_distance(const float* a, const float* b);
