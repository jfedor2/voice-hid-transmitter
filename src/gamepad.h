// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// Stadia controller buttons, in the order they are presented to the user.
enum class Button : int {
    A,
    B,
    X,
    Y,
    L1,
    R1,
    L2,
    R2,
    L3,
    R3,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Options,
    Menu,
    Stadia,
    Capture,
    Assistant,
    Count
};

constexpr int BUTTON_COUNT = (int) Button::Count;

enum class Stick : int {
    Left,
    Right,
    Count
};

constexpr int STICK_COUNT = (int) Stick::Count;

enum class Trigger : int {
    L2,
    R2,
    Count
};

constexpr int TRIGGER_COUNT = (int) Trigger::Count;

extern const char* const BUTTON_NAMES[BUTTON_COUNT];
extern const char* const STICK_NAMES[STICK_COUNT];        // "Left stick"
extern const char* const STICK_SHORT_NAMES[STICK_COUNT];  // "LS"
extern const char* const TRIGGER_NAMES[TRIGGER_COUNT];

// Display colors (RGB, 0-1).
struct Color3 {
    float r, g, b;
};
extern const Color3 BUTTON_COLORS[BUTTON_COUNT];
extern const Color3 STICK_COLORS[STICK_COUNT];

inline uint32_t button_mask(Button b) {
    return 1u << (int) b;
}

inline Button trigger_button(Trigger t) {
    return t == Trigger::L2 ? Button::L2 : Button::R2;
}

// What gets sent to the receiver. Sticks are -1..1 with -1 = left/up,
// triggers 0..1 (analog travel; a pressed L2/R2 button counts as full travel).
struct GamepadState {
    uint32_t buttons = 0;
    float sticks[STICK_COUNT][2] = {};
    float triggers[TRIGGER_COUNT] = {};

    bool operator==(const GamepadState& o) const;
    bool operator!=(const GamepadState& o) const { return !(*this == o); }
};

// Stick value as sent in the report: 0..255, 128 = center.
inline uint8_t stick_byte(float v) {
    int b = 128 + (int) (v * 127.0f + (v >= 0 ? 0.5f : -0.5f));
    return (uint8_t) (b < 1 ? 1 : (b > 255 ? 255 : b));
}

// Trigger value as sent in the report: 0..255.
inline uint8_t trigger_byte(float v) {
    int b = (int) (v * 255.0f + 0.5f);
    return (uint8_t) (b < 0 ? 0 : (b > 255 ? 255 : b));
}
