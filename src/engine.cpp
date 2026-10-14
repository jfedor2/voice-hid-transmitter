// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "engine.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "stadia.h"

static constexpr uint16_t RECEIVER_PORT = 42734;
static constexpr int TICK_MS = 4;
// UDP has no retransmission, so we repeat the current state periodically
// to recover from lost packets (and from the receiver rebooting when it
// switches to the Stadia descriptor).
static constexpr uint64_t KEEPALIVE_MS = 100;
static constexpr int NEUTRAL_RESENDS = 3;

// GetTickCount64() only has ~16 ms resolution, too coarse for button presses.
static uint64_t now_ms() {
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t) (c.QuadPart * 1000 / freq.QuadPart);
}

void ActionEngine::start() {
    quit_ = false;
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&ActionEngine::thread_main, this);
}

void ActionEngine::stop() {
    if (!thread_.joinable()) {
        return;
    }
    quit_ = true;
    SetEvent(wake_event_);
    thread_.join();
    CloseHandle(wake_event_);
    wake_event_ = nullptr;
}

void ActionEngine::set_actions(const std::map<uint32_t, Program>& programs) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_programs_ = programs;
    programs_pending_ = true;
}

void ActionEngine::set_active(bool active) {
    if (active_requested_.exchange(active) != active && wake_event_ != nullptr) {
        SetEvent(wake_event_);
    }
}

void ActionEngine::set_receiver(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_receiver_ = ip;
    receiver_pending_ = true;
}

void ActionEngine::post(const TriggerEvent& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }
    if (wake_event_ != nullptr) {
        SetEvent(wake_event_);
    }
}

GamepadState ActionEngine::current_state() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_state_;
}

std::vector<uint32_t> ActionEngine::busy_actions() {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_actions_;
}

void ActionEngine::sync_from_ui() {
    std::deque<TriggerEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (programs_pending_) {
            programs_ = std::move(pending_programs_);
            pending_programs_.clear();
            programs_pending_ = false;
            // Running instances keep their (old) program; a deleted action's
            // instance stops, but its latches stay until released.
            for (auto it = instances_.begin(); it != instances_.end();) {
                it = programs_.count(it->first) ? std::next(it) : instances_.erase(it);
            }
        }
        if (receiver_pending_) {
            receiver_pending_ = false;
            memset(&dest_, 0, sizeof(dest_));
            dest_.sin_family = AF_INET;
            dest_.sin_port = htons(RECEIVER_PORT);
            have_dest_ = inet_pton(AF_INET, pending_receiver_.c_str(), &dest_.sin_addr) == 1;
            address_valid_ = have_dest_;
            send_error_ = false;
        }
        events.swap(events_);
    }
    bool want_active = active_requested_.load();
    if (want_active != active_) {
        active_ = want_active;
        release_all();
        if (!active_) {
            neutral_resends_ = NEUTRAL_RESENDS;
        }
    }
    if (active_) {
        const uint64_t now = now_ms();
        for (const TriggerEvent& e : events) {
            handle_event(e, now);
        }
    }
}

void ActionEngine::handle_event(const TriggerEvent& event, uint64_t now) {
    const uint32_t id = event.action_id;
    if (event.type == TriggerType::Stop) {
        contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                                 [id](const Contribution& c) { return c.action_id == id && c.kind == Kind::Sound; }),
            contributions_.end());
        auto it = instances_.find(id);
        if (it != instances_.end()) {
            // If the program was at a "while" step, it goes on.
            it->second.sounding = false;
            if (it->second.in_while) {
                it->second.in_while = false;
                it->second.resume_at = now;
            }
        }
        return;
    }
    auto prog = programs_.find(id);
    if (prog == programs_.end()) {
        return;
    }
    // Retriggering restarts the action. Its timed presses run out on their own.
    contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                             [id](const Contribution& c) { return c.action_id == id && c.kind == Kind::Sound; }),
        contributions_.end());
    Instance inst;
    inst.program = prog->second;
    inst.resume_at = now;
    inst.continuous = event.type == TriggerType::Start;
    inst.sounding = inst.continuous;
    instances_[id] = std::move(inst);
}

void ActionEngine::add(uint32_t action_id, Kind kind, uint64_t until, const Target& t) {
    Contribution c;
    c.seq = next_seq_++;
    c.action_id = action_id;
    c.kind = kind;
    c.until = until;
    c.target = t;
    contributions_.push_back(c);
}

// Whether a contribution affects the same control as a target. L2/R2 the
// button and L2/R2 the analog trigger count as the same control here.
static bool same_control(const Target& a, const Target& b) {
    auto trigger_of = [](const Target& t) -> int {
        if (t.kind == TargetKind::Trigger) {
            return t.index;
        }
        if (t.kind == TargetKind::Button && t.index == (int) Button::L2) {
            return (int) Trigger::L2;
        }
        if (t.kind == TargetKind::Button && t.index == (int) Button::R2) {
            return (int) Trigger::R2;
        }
        return -1;
    };
    int ta = trigger_of(a), tb = trigger_of(b);
    if (ta >= 0 || tb >= 0) {
        return ta == tb;
    }
    return a.key() == b.key();
}

void ActionEngine::release_target(const Target& t) {
    contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                             [&](const Contribution& c) { return same_control(c.target, t); }),
        contributions_.end());
}

bool ActionEngine::is_latched(const Target& t) const {
    const Contribution* newest = nullptr;
    for (const Contribution& c : contributions_) {
        if (c.kind == Kind::Latch && c.target.key() == t.key() && (newest == nullptr || c.seq > newest->seq)) {
            newest = &c;
        }
    }
    if (newest == nullptr) {
        return false;
    }
    switch (t.kind) {
        case TargetKind::Stick:
            return std::fabs(newest->target.x - t.x) < 0.01f && std::fabs(newest->target.y - t.y) < 0.01f;
        case TargetKind::Trigger:
            return std::fabs(newest->target.value - t.value) < 0.01f;
        default:
            return true;
    }
}

void ActionEngine::release_all() {
    contributions_.clear();
    instances_.clear();
}

void ActionEngine::execute(uint32_t action_id, Instance& inst, const Step& step, uint64_t now) {
    switch (step.verb) {
        case Verb::Press: {
            const int ms = step.duration_ms > 0 ? step.duration_ms : DEFAULT_PRESS_MS;
            for (const Target& t : step.targets) {
                add(action_id, Kind::Timed, now + ms, t);
            }
            inst.resume_at = now + ms;
            break;
        }
        case Verb::Hold:
            for (const Target& t : step.targets) {
                // Replace an existing latch of the same control.
                contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                                         [&](const Contribution& c) { return c.kind == Kind::Latch && c.target.key() == t.key(); }),
                    contributions_.end());
                add(action_id, Kind::Latch, 0, t);
            }
            break;
        case Verb::Release:
            for (const Target& t : step.targets) {
                release_target(t);
            }
            break;
        case Verb::ReleaseAll:
            contributions_.clear();
            // Stop every other running action (the iteration in
            // run_instances() drops finished instances).
            for (auto& [id, other] : instances_) {
                if (&other != &inst) {
                    other.pc = other.program.steps.size();
                }
            }
            break;
        case Verb::Toggle: {
            bool all_latched = std::all_of(step.targets.begin(), step.targets.end(),
                [this](const Target& t) { return is_latched(t); });
            for (const Target& t : step.targets) {
                if (all_latched) {
                    release_target(t);
                } else {
                    contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                                             [&](const Contribution& c) { return c.kind == Kind::Latch && c.target.key() == t.key(); }),
                        contributions_.end());
                    add(action_id, Kind::Latch, 0, t);
                }
            }
            break;
        }
        case Verb::While:
            // Held until the sound stops, and the next step waits for that.
            // If the sound has already stopped, there's nothing to do.
            if (inst.sounding) {
                for (const Target& t : step.targets) {
                    add(action_id, Kind::Sound, 0, t);
                }
                inst.in_while = true;
            } else if (!inst.continuous) {
                // Fired by a whole sound (or a test): there's no "while", so
                // it's a default-length press.
                for (const Target& t : step.targets) {
                    add(action_id, Kind::Timed, now + DEFAULT_PRESS_MS, t);
                }
                inst.resume_at = now + DEFAULT_PRESS_MS;
            }
            break;
        case Verb::Wait:
            inst.resume_at = now + (step.duration_ms > 0 ? step.duration_ms : 0);
            break;
    }
}

void ActionEngine::run_instances(uint64_t now) {
    contributions_.erase(std::remove_if(contributions_.begin(), contributions_.end(),
                             [now](const Contribution& c) { return c.kind == Kind::Timed && c.until <= now; }),
        contributions_.end());
    for (auto& [id, inst] : instances_) {
        while (inst.pc < inst.program.steps.size() && now >= inst.resume_at && !inst.in_while) {
            const Step& step = inst.program.steps[inst.pc++];
            execute(id, inst, step, now);
        }
    }
    // Finished instances go away, unless a continuous sound is still holding
    // "while" contributions (it needs the instance to get the Stop).
    for (auto it = instances_.begin(); it != instances_.end();) {
        const bool done = it->second.pc >= it->second.program.steps.size() && !it->second.sounding;
        it = done ? instances_.erase(it) : std::next(it);
    }
}

void ActionEngine::compose_and_send() {
    GamepadState state;
    std::set<uint32_t> busy;
    if (active_) {
        const Contribution* newest_stick[STICK_COUNT] = {};
        const Contribution* newest_trigger[TRIGGER_COUNT] = {};
        for (const Contribution& c : contributions_) {
            busy.insert(c.action_id);
            const Target& t = c.target;
            switch (t.kind) {
                case TargetKind::Button:
                    state.buttons |= button_mask((Button) t.index);
                    break;
                case TargetKind::Stick:
                    if (newest_stick[t.index] == nullptr || c.seq > newest_stick[t.index]->seq) {
                        newest_stick[t.index] = &c;
                    }
                    break;
                case TargetKind::Trigger:
                    if (newest_trigger[t.index] == nullptr || c.seq > newest_trigger[t.index]->seq) {
                        newest_trigger[t.index] = &c;
                    }
                    break;
            }
        }
        for (int s = 0; s < STICK_COUNT; s++) {
            if (newest_stick[s] != nullptr) {
                state.sticks[s][0] = std::clamp(newest_stick[s]->target.x, -1.0f, 1.0f);
                state.sticks[s][1] = std::clamp(newest_stick[s]->target.y, -1.0f, 1.0f);
            }
        }
        for (int t = 0; t < TRIGGER_COUNT; t++) {
            if (newest_trigger[t] != nullptr) {
                state.triggers[t] = std::clamp(newest_trigger[t]->target.value, 0.0f, 1.0f);
            }
        }
        for (const auto& [id, inst] : instances_) {
            busy.insert(id);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_state_ = state;
        busy_actions_.assign(busy.begin(), busy.end());
    }

    const uint64_t now = now_ms();
    if (active_) {
        if (state != last_sent_ || now - last_send_ms_ >= KEEPALIVE_MS) {
            send_packet(state);
        }
    } else if (neutral_resends_ > 0) {
        if (state != last_sent_ || now - last_send_ms_ >= 20) {
            send_packet(GamepadState());
            neutral_resends_--;
        }
    }
}

void ActionEngine::send_packet(const GamepadState& state) {
    last_sent_ = state;
    last_send_ms_ = now_ms();
    if (!have_dest_ || socket_ == INVALID_SOCKET) {
        return;
    }
    uint8_t data[STADIA_PACKET_SIZE];
    size_t len = build_stadia_packet(state, data);
    int res = sendto(socket_, (const char*) data, (int) len, 0, (const sockaddr*) &dest_, sizeof(dest_));
    send_error_ = (res == SOCKET_ERROR);
    if (res != SOCKET_ERROR) {
        packets_sent_++;
    }
}

void ActionEngine::thread_main() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ != INVALID_SOCKET) {
        u_long nonblocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonblocking);
    }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * TICK_MS;
    SetWaitableTimer(timer, &due, TICK_MS, nullptr, nullptr, FALSE);

    HANDLE handles[2] = { timer, wake_event_ };
    while (!quit_) {
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        sync_from_ui();
        if (active_) {
            run_instances(now_ms());
        }
        compose_and_send();
    }

    // Leave the receiver with nothing pressed.
    if (active_) {
        active_ = false;
        release_all();
        compose_and_send();
        send_packet(GamepadState());
    }

    CloseHandle(timer);
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}
