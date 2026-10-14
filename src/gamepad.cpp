// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "gamepad.h"

const char* const BUTTON_NAMES[BUTTON_COUNT] = {
    "A", "B", "X", "Y",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "Up", "Down", "Left", "Right",
    "Options", "Menu", "Stadia", "Capture", "Assistant",
};

const char* const STICK_NAMES[STICK_COUNT] = {
    "Left stick",
    "Right stick",
};

const char* const STICK_SHORT_NAMES[STICK_COUNT] = {
    "LS",
    "RS",
};

const char* const TRIGGER_NAMES[TRIGGER_COUNT] = {
    "L2",
    "R2",
};

const Color3 BUTTON_COLORS[BUTTON_COUNT] = {
    { 0.20f, 0.75f, 0.30f },  // A
    { 0.85f, 0.25f, 0.25f },  // B
    { 0.25f, 0.45f, 0.90f },  // X
    { 0.90f, 0.75f, 0.15f },  // Y
    { 0.65f, 0.35f, 0.85f },  // L1
    { 0.65f, 0.35f, 0.85f },  // R1
    { 0.90f, 0.50f, 0.15f },  // L2
    { 0.90f, 0.50f, 0.15f },  // R2
    { 0.20f, 0.70f, 0.75f },  // L3
    { 0.20f, 0.70f, 0.75f },  // R3
    { 0.50f, 0.55f, 0.65f },  // Up
    { 0.50f, 0.55f, 0.65f },  // Down
    { 0.50f, 0.55f, 0.65f },  // Left
    { 0.50f, 0.55f, 0.65f },  // Right
    { 0.60f, 0.60f, 0.60f },  // Options
    { 0.60f, 0.60f, 0.60f },  // Menu
    { 0.95f, 0.35f, 0.55f },  // Stadia
    { 0.60f, 0.60f, 0.60f },  // Capture
    { 0.60f, 0.60f, 0.60f },  // Assistant
};

const Color3 STICK_COLORS[STICK_COUNT] = {
    { 0.15f, 0.60f, 0.85f },  // Left stick
    { 0.85f, 0.30f, 0.75f },  // Right stick
};

bool GamepadState::operator==(const GamepadState& o) const {
    if (buttons != o.buttons) {
        return false;
    }
    // Compare as sent, so changes too small to show up in the report don't count.
    for (int s = 0; s < STICK_COUNT; s++) {
        for (int a = 0; a < 2; a++) {
            if (stick_byte(sticks[s][a]) != stick_byte(o.sticks[s][a])) {
                return false;
            }
        }
    }
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        if (trigger_byte(triggers[t]) != trigger_byte(o.triggers[t])) {
            return false;
        }
    }
    return true;
}
