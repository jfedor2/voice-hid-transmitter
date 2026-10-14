// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "recognizer.h"

struct Sample {
    uint64_t id = 0;
    std::string file;  // file name in the samples directory
    std::shared_ptr<const Audio> audio;
};

struct Action {
    uint32_t id = 0;
    std::string name;
    std::string note;      // optional, for the user
    std::string notation;  // what it does
    bool enabled = true;  // off: as if the action didn't exist
    bool ignore = false;  // not a command: blocks sounds like its samples from triggering actions
    float margin = 0.0f;  // how much closer to its samples than to anything else a sound must be
    std::vector<Sample> samples;
};

struct WindowRect {
    bool valid = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;

    bool operator==(const WindowRect& o) const {
        return valid == o.valid && x == o.x && y == o.y && w == o.w && h == o.h && maximized == o.maximized;
    }
};

struct Config {
    std::string receiver_ip;
    std::string mic_id;      // empty = system default
    float vad_margin_db = 12.0f;
    int end_delay_ms = 150;
    WindowRect window;
    std::vector<Action> actions;
};

// %APPDATA%\voice-hid-transmitter\voice-hid-transmitter, created if needed.
std::wstring app_data_dir();
std::wstring samples_dir();

// Loads settings.json and the samples' audio.
bool load_config(const std::wstring& path, Config& config);
bool save_config(const std::wstring& path, const Config& config);

// 16 kHz mono 16-bit WAV.
bool write_wav(const std::wstring& path, const Audio& audio);
bool read_wav(const std::wstring& path, Audio& audio);
// In-memory WAV, e.g. for PlaySound(SND_MEMORY).
std::vector<uint8_t> wav_bytes(const Audio& audio);

uint64_t new_sample_id();
