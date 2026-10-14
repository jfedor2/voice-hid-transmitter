// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioDevice {
    std::string id;    // endpoint ID (UTF-8)
    std::string name;  // friendly name
};

// Active capture endpoints (microphones).
std::vector<AudioDevice> list_capture_devices();

// Captures 16 kHz mono float audio with WASAPI (shared mode, Windows does the
// format conversion) on its own thread and hands it to the sink. Reopens the
// device if it goes away, and follows the system default when no device is
// selected.
class AudioCapture {
public:
    using Sink = std::function<void(const float* samples, size_t n)>;
    // Called on the capture thread when the stream is (re)opened.
    using ResetFn = std::function<void()>;

    void start(Sink sink, ResetFn on_reset);
    void stop();

    // Empty = system default.
    void set_device(const std::string& id);

    // Name of the device being captured, or empty if none.
    std::string device_name();
    std::string error();

private:
    void thread_main();
    bool run_stream(const std::string& id);

    Sink sink_;
    ResetFn on_reset_;
    std::thread thread_;
    std::atomic<bool> quit_{ false };
    std::atomic<bool> device_changed_{ false };
    void* wake_event_ = nullptr;

    std::mutex mutex_;
    std::string requested_id_;
    std::string device_name_;
    std::string error_;
};
