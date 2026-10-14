// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "util.h"

using json = nlohmann::json;

// The same layout SDL_GetPrefPath() gives the other HID Forwarder tools.
std::wstring app_data_dir() {
    static std::wstring dir = []() {
        PWSTR appdata = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appdata))) {
            CoTaskMemFree(appdata);
            return std::wstring(L".");
        }
        std::wstring path = appdata;
        CoTaskMemFree(appdata);
        for (const wchar_t* part : { L"\\voice-hid-transmitter", L"\\voice-hid-transmitter" }) {
            path += part;
            CreateDirectoryW(path.c_str(), nullptr);
        }
        return path;
    }();
    return dir;
}

static std::wstring sub_dir(const wchar_t* name) {
    std::wstring dir = app_data_dir() + L"\\" + name;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring samples_dir() {
    return sub_dir(L"samples");
}

uint64_t new_sample_id() {
    // Time-based, so ids stay unique across runs without being stored.
    static std::atomic<uint64_t> last{ 0 };
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t) ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10;  // microseconds
    uint64_t prev = last.load();
    while (true) {
        uint64_t next = std::max(t, prev + 1);
        if (last.compare_exchange_weak(prev, next)) {
            return next;
        }
    }
}

bool load_config(const std::wstring& path, Config& config) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    try {
        json j = json::parse(f);
        config.receiver_ip = j.value("receiver_ip", "");
        config.mic_id = j.value("mic_id", "");
        config.vad_margin_db = std::clamp(j.value("vad_margin_db", 12.0f), 3.0f, 40.0f);
        config.end_delay_ms = std::clamp(j.value("end_delay_ms", 150), 50, 500);
        if (j.contains("window") && j["window"].is_object()) {
            const json& jw = j["window"];
            config.window.x = jw.value("x", 0);
            config.window.y = jw.value("y", 0);
            config.window.w = jw.value("w", 0);
            config.window.h = jw.value("h", 0);
            config.window.maximized = jw.value("maximized", false);
            config.window.valid = config.window.w > 0 && config.window.h > 0;
        }
        config.actions.clear();
        uint32_t next_id = 1;
        const std::wstring dir = samples_dir();
        for (const auto& ja : j.value("actions", json::array())) {
            Action a;
            a.id = next_id++;
            a.name = ja.value("name", "");
            a.note = ja.value("note", "");
            a.notation = ja.value("notation", "");
            a.enabled = ja.value("enabled", true);
            a.ignore = ja.value("ignore", ja.value("detect", "") == "Ignore");
            a.margin = std::clamp(ja.value("margin", 0.0f), -0.1f, 0.2f);
            for (const auto& js : ja.value("samples", json::array())) {
                if (!js.is_string()) {
                    continue;
                }
                Sample s;
                s.id = new_sample_id();
                s.file = js.get<std::string>();
                auto audio = std::make_shared<Audio>();
                if (read_wav(dir + L"\\" + from_utf8(s.file), *audio)) {
                    s.audio = audio;
                    a.samples.push_back(s);
                }
            }
            config.actions.push_back(std::move(a));
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool save_config(const std::wstring& path, const Config& config) {
    json j;
    j["receiver_ip"] = config.receiver_ip;
    j["mic_id"] = config.mic_id;
    j["vad_margin_db"] = config.vad_margin_db;
    j["end_delay_ms"] = config.end_delay_ms;
    if (config.window.valid) {
        j["window"] = {
            { "x", config.window.x },
            { "y", config.window.y },
            { "w", config.window.w },
            { "h", config.window.h },
            { "maximized", config.window.maximized },
        };
    }
    json actions = json::array();
    for (const Action& a : config.actions) {
        json samples = json::array();
        for (const Sample& s : a.samples) {
            samples.push_back(s.file);
        }
        actions.push_back({
            { "name", a.name },
            { "note", a.note },
            { "notation", a.notation },
            { "enabled", a.enabled },
            { "ignore", a.ignore },
            { "margin", a.margin },
            { "samples", samples },
        });
    }
    j["actions"] = actions;

    std::wstring tmp_path = path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(2);
        if (!f) {
            return false;
        }
    }
    return MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// ---------------------------------------------------------------------------
// WAV

std::vector<uint8_t> wav_bytes(const Audio& audio) {
    const uint32_t data_size = (uint32_t) audio.size() * 2;
    std::vector<uint8_t> out(44 + data_size);
    uint8_t* p = out.data();
    auto put32 = [&p](uint32_t v) {
        memcpy(p, &v, 4);
        p += 4;
    };
    auto put16 = [&p](uint16_t v) {
        memcpy(p, &v, 2);
        p += 2;
    };
    auto tag = [&p](const char* t) {
        memcpy(p, t, 4);
        p += 4;
    };
    tag("RIFF");
    put32(36 + data_size);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);  // PCM
    put16(1);  // mono
    put32(SAMPLE_RATE);
    put32(SAMPLE_RATE * 2);
    put16(2);
    put16(16);
    tag("data");
    put32(data_size);
    for (float s : audio) {
        int v = (int) std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        put16((uint16_t) (int16_t) v);
    }
    return out;
}

bool write_wav(const std::wstring& path, const Audio& audio) {
    std::vector<uint8_t> bytes = wav_bytes(audio);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*) bytes.data(), (std::streamsize) bytes.size());
    return (bool) f;
}

// Reads 16-bit PCM WAV files at 16 kHz (mono, or the first channel).
bool read_wav(const std::wstring& path, Audio& audio) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        uint32_t size;
        memcpy(&size, &bytes[pos + 4], 4);
        const uint8_t* body = &bytes[pos + 8];
        if (pos + 8 + size > bytes.size()) {
            size = (uint32_t) (bytes.size() - pos - 8);
        }
        if (memcmp(&bytes[pos], "fmt ", 4) == 0 && size >= 16) {
            memcpy(&format, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
        } else if (memcmp(&bytes[pos], "data", 4) == 0) {
            if (format != 1 || bits != 16 || rate != SAMPLE_RATE || channels == 0) {
                return false;
            }
            const size_t frames = size / (2 * channels);
            audio.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                int16_t v;
                memcpy(&v, body + i * 2 * channels, 2);
                audio[i] = v / 32768.0f;
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}
