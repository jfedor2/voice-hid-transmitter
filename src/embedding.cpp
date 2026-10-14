// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "embedding.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>

namespace speech {

namespace {

constexpr int SAMPLE_RATE = 16000;
constexpr int FFT_SIZE = 512;
constexpr int HOP = 160;
constexpr int WINDOW = 400;  // a Hann window this long, centered in the FFT
constexpr float MEL_LOW_HZ = 60.0f;
constexpr float MEL_HIGH_HZ = 3800.0f;
constexpr float TOP_DB = 80.0f;
constexpr float LEAKY_SLOPE = 0.2f;
constexpr float ACTIVATION_FLOOR = -0.4f;
constexpr int CLIP_STRIDE = 8;  // frames between windows of a long clip
constexpr double PI = 3.14159265358979323846;

// The spectrogram front end the model was trained with (torch's
// melspectrogram with fixed parameters): 400-sample periodic Hann window in a
// 512-point FFT, power spectrum, 32 Slaney-style mel bands from 60 to 3800 Hz
// (area normalized), dB clipped 80 dB below the maximum.
struct MelTables {
    float window[FFT_SIZE] = {};
    std::complex<float> twiddle[FFT_SIZE / 2];
    int bitrev[FFT_SIZE];
    std::vector<float> filters;  // MEL_BINS x (FFT_SIZE / 2 + 1)

    MelTables() {
        const int offset = (FFT_SIZE - WINDOW) / 2;
        for (int i = 0; i < WINDOW; i++) {
            window[offset + i] = (float) (0.5 - 0.5 * std::cos(2 * PI * i / WINDOW));
        }
        for (int i = 0; i < FFT_SIZE / 2; i++) {
            twiddle[i] = std::polar(1.0f, (float) (-2 * PI * i / FFT_SIZE));
        }
        for (int i = 0; i < FFT_SIZE; i++) {
            int r = 0;
            for (int b = 0; b < 9; b++) {
                r |= ((i >> b) & 1) << (8 - b);
            }
            bitrev[i] = r;
        }

        // Slaney mel scale: linear below 1 kHz, logarithmic above.
        const double f_sp = 200.0 / 3, min_log_mel = 1000.0 / f_sp, log_step = std::log(6.4) / 27;
        auto hz_to_mel = [&](double hz) { return hz < 1000 ? hz / f_sp : min_log_mel + std::log(hz / 1000) / log_step; };
        auto mel_to_hz = [&](double m) { return m < min_log_mel ? m * f_sp : 1000 * std::exp(log_step * (m - min_log_mel)); };
        double points[MEL_BINS + 2];
        const double lo = hz_to_mel(MEL_LOW_HZ), hi = hz_to_mel(MEL_HIGH_HZ);
        for (int i = 0; i < MEL_BINS + 2; i++) {
            points[i] = mel_to_hz(lo + (hi - lo) * i / (MEL_BINS + 1));
        }
        const int bins = FFT_SIZE / 2 + 1;
        filters.assign(MEL_BINS * bins, 0.0f);
        for (int m = 0; m < MEL_BINS; m++) {
            const double l = points[m], c = points[m + 1], r = points[m + 2];
            for (int k = 0; k < bins; k++) {
                const double f = k * (double) SAMPLE_RATE / FFT_SIZE;
                const double w = std::max(0.0, std::min((f - l) / (c - l), (r - f) / (r - c)));
                filters[m * bins + k] = (float) (w * 2.0 / (r - l));
            }
        }
    }
};

const MelTables& mel_tables() {
    static MelTables t;
    return t;
}

void fft(std::complex<float>* x) {
    const MelTables& t = mel_tables();
    for (int i = 0; i < FFT_SIZE; i++) {
        const int j = t.bitrev[i];
        if (j > i) {
            std::swap(x[i], x[j]);
        }
    }
    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        const int half = len / 2, step = FFT_SIZE / len;
        for (int i = 0; i < FFT_SIZE; i += len) {
            for (int k = 0; k < half; k++) {
                const std::complex<float> w = t.twiddle[k * step] * x[i + k + half];
                x[i + k + half] = x[i + k] - w;
                x[i + k] += w;
            }
        }
    }
}

// Quiet filler for padding clips, a fixed pseudo-random sequence at about -80 dBFS.
void fill_background(float* out, size_t n, uint32_t& seed) {
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        out[i] = ((seed >> 8) / 16777216.0f - 0.5f) * 0.0003f;
    }
}

}  // namespace

bool Embedder::load(const void* data, size_t size) {
    layers_.clear();
    const uint8_t* p = (const uint8_t*) data;
    const uint8_t* end = p + size;
    auto read = [&](void* dst, size_t n) {
        if ((size_t) (end - p) < n) {
            return false;
        }
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    char magic[4];
    uint32_t count = 0;
    if (!read(magic, 4) || memcmp(magic, "VHTE", 4) != 0 || !read(&count, 4) || count != 20) {
        return false;
    }
    std::vector<Layer> layers(count);
    for (Layer& l : layers) {
        int32_t header[5];
        if (!read(header, sizeof(header))) {
            return false;
        }
        l.out = header[0];
        l.in = header[1];
        l.kh = header[2];
        l.kw = header[3];
        std::vector<float> oihw((size_t) l.out * l.in * l.kh * l.kw);
        if (!read(oihw.data(), oihw.size() * sizeof(float))) {
            return false;
        }
        // Reorder so the innermost loop of the convolution runs over output
        // channels, which vectorizes well even when the image is tiny.
        l.weights.resize(oihw.size());
        for (int o = 0; o < l.out; o++) {
            for (int i = 0; i < l.in; i++) {
                for (int y = 0; y < l.kh; y++) {
                    for (int x = 0; x < l.kw; x++) {
                        l.weights[(((size_t) y * l.kw + x) * l.in + i) * l.out + o] =
                            oihw[(((size_t) o * l.in + i) * l.kh + y) * l.kw + x];
                    }
                }
            }
        }
        if (header[4]) {
            l.bias.resize(l.out);
            if (!read(l.bias.data(), l.bias.size() * sizeof(float))) {
                return false;
            }
        }
    }
    layers_ = std::move(layers);
    return true;
}

std::vector<float> Embedder::mel(const float* audio, size_t n) const {
    const MelTables& t = mel_tables();
    if (n < FFT_SIZE) {
        return {};
    }
    const size_t frames = (n - FFT_SIZE) / HOP + 1;
    const int bins = FFT_SIZE / 2 + 1;
    std::vector<float> out(frames * MEL_BINS);
    std::complex<float> x[FFT_SIZE];
    float power[FFT_SIZE / 2 + 1];
    float top = -1e30f;
    for (size_t f = 0; f < frames; f++) {
        const float* src = audio + f * HOP;
        for (int i = 0; i < FFT_SIZE; i++) {
            x[i] = src[i] * 32768.0f * t.window[i];  // the model works on 16-bit sample values
        }
        fft(x);
        for (int k = 0; k < bins; k++) {
            power[k] = std::norm(x[k]);
        }
        for (int m = 0; m < MEL_BINS; m++) {
            const float* w = &t.filters[m * bins];
            float e = 0;
            for (int k = 0; k < bins; k++) {
                e += w[k] * power[k];
            }
            const float db = 10.0f * std::log10(std::max(e, 1e-10f));
            out[f * MEL_BINS + m] = db;
            top = std::max(top, db);
        }
    }
    for (float& v : out) {
        v = std::max(v, top - TOP_DB) / 10.0f + 2.0f;
    }
    return out;
}

Embedding Embedder::embed(const float* mel) const {
    // Height (time) x width (mel bands) x channels.
    int c = 1, h = EMBED_FRAMES, w = MEL_BINS;
    std::vector<float> cur(mel, mel + EMBED_FRAMES * MEL_BINS), next;
    // Max pooling after these layers, (height, width).
    struct Pool {
        int after, ph, pw;
    };
    static const Pool pools[] = { { 2, 2, 2 }, { 6, 1, 2 }, { 10, 2, 2 }, { 14, 1, 2 }, { 18, 2, 2 } };

    for (size_t li = 0; li < layers_.size(); li++) {
        const Layer& L = layers_[li];
        // Convolution: no padding in time, "same" padding across mel bands.
        const int pad = L.kw == 3 ? 1 : 0;
        const int oh = h - L.kh + 1, ow = w + 2 * pad - L.kw + 1;
        // Locals, so the compiler knows the loop bounds don't change (and vectorizes).
        const int n_in = L.in, n_out = L.out;
        next.assign((size_t) oh * ow * n_out, 0.0f);
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                float* __restrict dst = &next[((size_t) y * ow + x) * n_out];
                if (!L.bias.empty()) {
                    std::copy(L.bias.begin(), L.bias.end(), dst);
                }
                for (int ky = 0; ky < L.kh; ky++) {
                    for (int kx = 0; kx < L.kw; kx++) {
                        const int sx = x + kx - pad;
                        if (sx < 0 || sx >= w) {
                            continue;
                        }
                        const float* src = &cur[((size_t) (y + ky) * w + sx) * n_in];
                        const float* wk = &L.weights[((size_t) ky * L.kw + kx) * n_in * n_out];
                        for (int i = 0; i < n_in; i++) {
                            const float s = src[i];
                            const float* __restrict wi = wk + (size_t) i * n_out;
                            for (int o = 0; o < n_out; o++) {
                                dst[o] += s * wi[o];
                            }
                        }
                    }
                }
            }
        }
        c = L.out;
        h = oh;
        w = ow;
        cur.swap(next);
        if (li + 1 == layers_.size()) {
            break;  // the last layer has no activation
        }
        for (float& v : cur) {
            v = std::max(v > 0 ? v : v * LEAKY_SLOPE, ACTIVATION_FLOOR);
        }
        for (const Pool& p : pools) {
            if (p.after != (int) li) {
                continue;
            }
            const int ph = h / p.ph, pw = w / p.pw;
            next.assign((size_t) ph * pw * c, -1e30f);
            for (int y = 0; y < ph; y++) {
                for (int x = 0; x < pw; x++) {
                    float* dst = &next[((size_t) y * pw + x) * c];
                    for (int dy = 0; dy < p.ph; dy++) {
                        for (int dx = 0; dx < p.pw; dx++) {
                            const float* src = &cur[((size_t) (y * p.ph + dy) * w + x * p.pw + dx) * c];
                            for (int ch = 0; ch < c; ch++) {
                                dst[ch] = std::max(dst[ch], src[ch]);
                            }
                        }
                    }
                }
            }
            h = ph;
            w = pw;
            cur.swap(next);
        }
    }
    Embedding out{};
    for (int i = 0; i < EMBED_DIM && i < (int) cur.size(); i++) {
        out[i] = cur[i];
    }
    return out;
}

std::vector<float> trim_to_loud(const std::vector<float>& audio) {
    const size_t frames = audio.size() / HOP;
    if (frames < 3) {
        return audio;
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
    const float threshold = std::max(top - 35.0f, -65.0f);
    size_t first = frames, last = 0;
    for (size_t f = 0; f < frames; f++) {
        if (db[f] > threshold) {
            first = std::min(first, f);
            last = f;
        }
    }
    if (first == frames) {
        return audio;
    }
    const size_t a = (first >= 3 ? first - 3 : 0) * HOP;
    const size_t b = std::min(frames, last + 4) * HOP;
    return std::vector<float>(audio.begin() + a, audio.begin() + b);
}

std::vector<Embedding> Embedder::embed_clip(const std::vector<float>& audio) const {
    std::vector<Embedding> out;
    if (!loaded()) {
        return out;
    }
    const std::vector<float> loud = trim_to_loud(audio);
    uint32_t seed = 12345;
    const size_t need = EMBED_FRAMES * HOP + 3 * HOP;  // samples for EMBED_FRAMES mel frames
    if (loud.size() < need) {
        // Center the sound in one window, with quiet around it.
        std::vector<float> x(need);
        const size_t before = (need - loud.size()) / 2;
        fill_background(x.data(), before, seed);
        std::copy(loud.begin(), loud.end(), x.begin() + before);
        fill_background(x.data() + before + loud.size(), need - before - loud.size(), seed);
        std::vector<float> m = mel(x.data(), x.size());
        m.resize(EMBED_FRAMES * MEL_BINS, m.empty() ? 0.0f : m[m.size() - 1]);
        out.push_back(embed(m.data()));
        return out;
    }
    const size_t margin = 5 * HOP;
    std::vector<float> x(loud.size() + 2 * margin);
    fill_background(x.data(), margin, seed);
    std::copy(loud.begin(), loud.end(), x.begin() + margin);
    fill_background(x.data() + margin + loud.size(), margin, seed);
    const std::vector<float> m = mel(x.data(), x.size());
    const int frames = (int) (m.size() / MEL_BINS);
    for (int f = 0; f + EMBED_FRAMES <= frames; f += CLIP_STRIDE) {
        out.push_back(embed(&m[(size_t) f * MEL_BINS]));
    }
    return out;
}

Embedding Embedder::embed_tail(const std::vector<float>& audio) const {
    if (!loaded()) {
        return Embedding{};
    }
    const size_t need = EMBED_FRAMES * HOP + 3 * HOP;  // samples for EMBED_FRAMES mel frames
    const size_t n = std::min(need, audio.size());
    std::vector<float> x(need);
    uint32_t seed = 12345;
    fill_background(x.data(), need - n, seed);
    std::copy(audio.end() - n, audio.end(), x.begin() + (need - n));
    std::vector<float> m = mel(x.data(), x.size());
    m.resize(EMBED_FRAMES * MEL_BINS, m.empty() ? 0.0f : m[m.size() - 1]);
    return embed(m.data());
}

const Embedder& embedder() {
    // Never destroyed: threads may still be using it while the program exits.
    static const Embedder& e = *[]() {
        Embedder* em = new Embedder();
        HMODULE module = GetModuleHandleW(nullptr);
        if (HRSRC res = FindResourceW(module, L"SPEECH_EMBEDDING", RT_RCDATA)) {
            if (HGLOBAL h = LoadResource(module, res)) {
                em->load(LockResource(h), SizeofResource(module, res));
            }
        }
        return em;
    }();
    return e;
}

}  // namespace speech
