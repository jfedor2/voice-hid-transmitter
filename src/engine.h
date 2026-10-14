// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gamepad.h"
#include "notation.h"

// What the recognizer tells the engine about an action's sound.
enum class TriggerType {
    Fire,   // a whole sound was recognized (or the action was tested)
    Start,  // a continuous sound started: "while" steps stay active...
    Stop,   // ...until it stops
};

struct TriggerEvent {
    uint32_t action_id = 0;
    TriggerType type = TriggerType::Fire;
};

// Runs on its own thread: executes action programs, composes the gamepad
// report and sends it to the receiver over UDP.
//
// Outputs come from contributions: latched ones (hold/toggle), timed ones
// (press) and ones that last while a sound does (while). Buttons from all
// contributions are OR'd. For a stick or an analog trigger the most recent
// active contribution wins, so a new position overrides the previous one, and
// when it ends the previous one (if still active) comes back.
class ActionEngine {
public:
    void start();
    void stop();

    // Called from the UI thread whenever actions are edited.
    void set_actions(const std::map<uint32_t, Program>& programs);
    // Active = actions run and reports are sent. When inactive, everything is released.
    void set_active(bool active);
    void set_receiver(const std::string& ip);

    // Thread safe, cheap; called from the audio thread.
    void post(const TriggerEvent& event);

    GamepadState current_state();
    // Actions currently running a program or contributing to the output.
    std::vector<uint32_t> busy_actions();

    bool address_valid() const { return address_valid_.load(); }
    bool send_error() const { return send_error_.load(); }
    uint64_t packets_sent() const { return packets_sent_.load(); }

private:
    enum class Kind {
        Latch,   // until released
        Timed,   // until `until`
        Sound,   // until the action's sound stops
    };

    struct Contribution {
        uint64_t seq = 0;  // creation order, newest wins for sticks and triggers
        uint32_t action_id = 0;
        Kind kind = Kind::Latch;
        uint64_t until = 0;
        Target target;
    };

    struct Instance {
        Program program;
        size_t pc = 0;
        uint64_t resume_at = 0;
        bool sounding = false;    // started by a continuous sound that hasn't stopped
        bool continuous = false;  // started by Start rather than Fire
        bool in_while = false;    // at a "while" step, waiting for the sound to stop
    };

    void thread_main();
    void sync_from_ui();
    void handle_event(const TriggerEvent& event, uint64_t now);
    void run_instances(uint64_t now);
    void execute(uint32_t action_id, Instance& inst, const Step& step, uint64_t now);
    void add(uint32_t action_id, Kind kind, uint64_t until, const Target& t);
    void release_target(const Target& t);
    bool is_latched(const Target& t) const;
    void release_all();
    void compose_and_send();
    void send_packet(const GamepadState& state);

    std::thread thread_;
    std::atomic<bool> quit_{ false };
    HANDLE wake_event_ = nullptr;

    // Shared with other threads.
    std::mutex mutex_;
    std::map<uint32_t, Program> pending_programs_;
    bool programs_pending_ = false;
    std::string pending_receiver_;
    bool receiver_pending_ = false;
    std::deque<TriggerEvent> events_;
    std::atomic<bool> active_requested_{ false };
    GamepadState current_state_;           // guarded by mutex_
    std::vector<uint32_t> busy_actions_;   // guarded by mutex_

    std::atomic<bool> address_valid_{ false };
    std::atomic<bool> send_error_{ false };
    std::atomic<uint64_t> packets_sent_{ 0 };

    // Engine thread only.
    std::map<uint32_t, Program> programs_;
    std::map<uint32_t, Instance> instances_;
    std::vector<Contribution> contributions_;
    uint64_t next_seq_ = 1;
    bool active_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in dest_{};
    bool have_dest_ = false;
    GamepadState last_sent_;
    uint64_t last_send_ms_ = 0;
    int neutral_resends_ = 0;
};
