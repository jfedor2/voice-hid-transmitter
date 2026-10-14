// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#include "gamepad.h"

// The formal notation for what an action does. A program is a list of steps
// separated by ';' (or newlines), executed in order:
//
//   press A               press for the default time (100 ms), then go on
//   press A+B 200ms       several targets at once, for a given time
//   hold X                latch on until released
//   release X             unlatch (sticks go back to center, triggers to 0)
//   release all           release everything, stop all running actions
//   toggle L3             latch if not latched, otherwise release
//   while A               active for as long as the sound lasts, then go on
//   wait 150ms            pause before the next step
//
// Targets are buttons (A B X Y L1 R1 L2 R2 L3 R3 UP DOWN LEFT RIGHT OPTIONS
// MENU STADIA CAPTURE ASSISTANT), stick positions (LS=NE, RS=N*0.5,
// LS=(0.3,-1) with +y = up) and analog trigger values (L2=0.5).
// "press" waits for its duration before the next step starts, "while" for
// the sound to stop (and is skipped if it already has); all other steps
// except "wait" are instant.

enum class Verb {
    Press,
    Hold,
    Release,
    Toggle,
    While,
    Wait,
    ReleaseAll,
};

enum class TargetKind {
    Button,
    Stick,    // a position (or, for release, the whole stick)
    Trigger,  // an analog value (or, for release, the whole trigger)
};

// Stick directions, clockwise from north. Center is DIR_CENTER, an explicit
// (x,y) position is DIR_EXPLICIT.
constexpr int DIR_COUNT = 8;
constexpr int DIR_CENTER = 8;
constexpr int DIR_EXPLICIT = -1;
extern const char* const DIR_NAMES[DIR_COUNT + 1];  // N NE E SE S SW W NW C

struct Target {
    TargetKind kind = TargetKind::Button;
    int index = 0;  // Button, Stick or Trigger
    bool has_value = false;  // sticks and triggers: false only in "release LS"
    // Stick: direction and magnitude, or an explicit position.
    int dir = DIR_EXPLICIT;
    float mag = 1.0f;
    float x = 0.0f;  // stick position as sent, -1..1, y: -1 = up
    float y = 0.0f;
    float value = 0.0f;  // trigger travel, 0..1

    // Targets with the same key affect the same control.
    int key() const { return (int) kind * 100 + index; }
};

struct Step {
    Verb verb = Verb::Press;
    std::vector<Target> targets;
    int duration_ms = -1;  // Press, Wait; -1 = default
};

constexpr int DEFAULT_PRESS_MS = 100;

struct Program {
    std::vector<Step> steps;

    bool uses_while() const;
};

// Targets as the parser would produce them.
Target button_target(Button b);
Target stick_target(Stick s, int dir, float mag);  // dir: 0..DIR_COUNT-1 or DIR_CENTER
Target stick_release_target(Stick s);              // the whole stick, for "release"
Target trigger_target(Trigger t, float value);

// Parses notation, case-insensitively and with some aliases (LB, RT, START,
// "north", ...). On failure, error describes the problem.
bool parse_program(const std::string& text, Program& out, std::string& error);

// Canonical text of a program, e.g. "press A+B 200ms; hold LS=NE".
std::string format_program(const Program& program);
