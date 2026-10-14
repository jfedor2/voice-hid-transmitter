// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "features.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace {

constexpr int FFT_SIZE = 512;
constexpr int NUM_MEL = 32;
constexpr float MEL_LOW_HZ = 60.0f;
constexpr float MEL_HIGH_HZ = 7800.0f;
constexpr float PRE_EMPHASIS = 0.97f;
constexpr float PI = 3.14159265358979f;

struct Tables {
    float window[WINDOW];
    std::complex<float> twiddle[FFT_SIZE / 2];
    int bitrev[FFT_SIZE];
    // Triangular mel filters over the FFT bins 0..FFT_SIZE/2.
    struct Filter {
        int first;
        std::vector<float> weights;
    } mel[NUM_MEL];
    float dct[NUM_CEPS][NUM_MEL];

    Tables() {
        for (int i = 0; i < WINDOW; i++) {
            window[i] = 0.54f - 0.46f * std::cos(2 * PI * i / (WINDOW - 1));
        }
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            twiddle[i] = std::polar(1.0f, -2 * PI * i / FFT_SIZE);
        }
        int bits = 0;
        while ((1 << bits) < FFT_SIZE) {
            bits++;
        }
        for (int i = 0; i < FFT_SIZE; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) {
                r |= ((i >> b) & 1) << (bits - 1 - b);
            }
            bitrev[i] = r;
        }

        auto hz_to_mel = [](float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); };
        auto mel_to_hz = [](float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); };
        const float lo = hz_to_mel(MEL_LOW_HZ), hi = hz_to_mel(MEL_HIGH_HZ);
        float edges[NUM_MEL + 2];
        for (int i = 0; i < NUM_MEL + 2; i++) {
            edges[i] = mel_to_hz(lo + (hi - lo) * i / (NUM_MEL + 1)) * FFT_SIZE / SAMPLE_RATE;  // in bins
        }
        for (int m = 0; m < NUM_MEL; m++) {
            const float l = edges[m], c = edges[m + 1], r = edges[m + 2];
            Filter& f = mel[m];
            f.first = (int) std::ceil(l);
            for (int k = f.first; k <= (int) std::floor(r) && k <= FFT_SIZE / 2; k++) {
                float w = k <= c ? (k - l) / (c - l) : (r - k) / (r - c);
                f.weights.push_back(std::max(0.0f, w));
            }
            if (f.weights.empty()) {
                // Very narrow low filters can fall between bins: use the nearest one.
                f.first = (int) std::lround(c);
                f.weights.push_back(1.0f);
            }
        }

        // DCT-II of the log mel energies, c1..c12, with a sinusoidal lifter
        // so the coefficients have more comparable ranges.
        // A mild lifter: stronger ones (the usual 22) overweight the noisy
        // high coefficients of short sounds like clicks.
        const float lifter = 6.0f;
        for (int k = 0; k < NUM_CEPS; k++) {
            const int n = k + 1;
            const float lift = 1.0f + lifter / 2 * std::sin(PI * n / lifter);
            for (int m = 0; m < NUM_MEL; m++) {
                dct[k][m] = std::sqrt(2.0f / NUM_MEL) * std::cos(PI * n * (m + 0.5f) / NUM_MEL) * lift;
            }
        }
    }
};

const Tables& tables() {
    static Tables t;
    return t;
}

void fft(std::complex<float>* x) {
    const Tables& t = tables();
    for (int i = 0; i < FFT_SIZE; i++) {
        int j = t.bitrev[i];
        if (j > i) {
            std::swap(x[i], x[j]);
        }
    }
    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        const int half = len / 2, step = FFT_SIZE / len;
        for (int i = 0; i < FFT_SIZE; i += len) {
            for (int k = 0; k < half; k++) {
                std::complex<float> w = t.twiddle[k * step] * x[i + k + half];
                x[i + k + half] = x[i + k] - w;
                x[i + k] += w;
            }
        }
    }
}

}  // namespace

FeatureFrontend::FeatureFrontend() {
    reset();
}

void FeatureFrontend::reset() {
    buffer_.assign(WINDOW * 2, 0.0f);  // [0, WINDOW): pre-emphasized, [WINDOW, 2*WINDOW): raw
    filled_ = 0;
    prev_sample_ = 0.0f;
}

void FeatureFrontend::process(const float* samples, size_t n, std::vector<Frame>& out) {
    float* emph = buffer_.data();
    float* raw = buffer_.data() + WINDOW;
    for (size_t i = 0; i < n; i++) {
        const float s = samples[i];
        emph[filled_] = s - PRE_EMPHASIS * prev_sample_;
        raw[filled_] = s;
        filled_++;
        prev_sample_ = s;
        if (filled_ == WINDOW) {
            out.emplace_back();
            compute_frame(out.back());
            // Slide the window by one hop.
            memmove(emph, emph + HOP, (WINDOW - HOP) * sizeof(float));
            memmove(raw, raw + HOP, (WINDOW - HOP) * sizeof(float));
            filled_ -= HOP;
        }
    }
}

void FeatureFrontend::compute_frame(Frame& out) {
    const Tables& t = tables();
    const float* emph = buffer_.data();
    const float* raw = buffer_.data() + WINDOW;

    double sum_sq = 0;
    for (int i = 0; i < WINDOW; i++) {
        sum_sq += (double) raw[i] * raw[i];
    }
    out.energy_db = (float) (10.0 * std::log10(sum_sq / WINDOW + 1e-10));

    std::complex<float> x[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        x[i] = i < WINDOW ? emph[i] * t.window[i] : 0.0f;
    }
    fft(x);
    float power[FFT_SIZE / 2 + 1];
    for (int k = 0; k <= FFT_SIZE / 2; k++) {
        power[k] = std::norm(x[k]);
    }
    float log_mel[NUM_MEL];
    for (int m = 0; m < NUM_MEL; m++) {
        const auto& f = t.mel[m];
        float e = 0;
        for (size_t j = 0; j < f.weights.size(); j++) {
            e += f.weights[j] * power[f.first + j];
        }
        log_mel[m] = std::log(e + 1e-8f);
    }
    for (int k = 0; k < NUM_CEPS; k++) {
        float c = 0;
        for (int m = 0; m < NUM_MEL; m++) {
            c += t.dct[k][m] * log_mel[m];
        }
        out.ceps[k] = c;
    }
}

std::vector<Frame> FeatureFrontend::frames_of(const std::vector<float>& audio) {
    FeatureFrontend fe;
    std::vector<Frame> frames;
    fe.process(audio.data(), audio.size(), frames);
    return frames;
}

static float vec_distance(const float* a, const float* b, int dim) {
    float s = 0;
    for (int i = 0; i < dim; i++) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return std::sqrt(s);
}

float ceps_distance(const float* a, const float* b) {
    return vec_distance(a, b, NUM_CEPS);
}
