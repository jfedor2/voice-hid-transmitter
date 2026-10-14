// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

#include "gamepad.h"

// HID Forwarder packet carrying a Stadia controller report.
// Layout matches build_stadia_report() in hid-forwarder's transmitter-sdl.
constexpr size_t STADIA_PACKET_SIZE = 14;

inline size_t build_stadia_packet(const GamepadState& state, uint8_t* data) {
    const uint32_t buttons = state.buttons;
    auto pressed = [buttons](Button b) -> uint8_t { return (buttons & button_mask(b)) ? 1 : 0; };

    // A pressed L2/R2 button means full travel, otherwise the analog value is used.
    // The digital bit follows the analog value, with the same threshold as
    // hid-forwarder's transmitter-sdl.
    auto trigger_value = [&](Trigger t) -> uint8_t {
        return pressed(trigger_button(t)) ? 255 : trigger_byte(state.triggers[(int) t]);
    };
    const uint8_t l2 = trigger_value(Trigger::L2);
    const uint8_t r2 = trigger_value(Trigger::R2);

    data[0] = 1;   // protocol version
    data[1] = 4;   // Stadia descriptor number
    data[2] = 10;  // report length
    data[3] = 3;   // report ID

    static const uint8_t dpad_lut[16] = { 8, 6, 2, 8, 0, 7, 1, 0, 4, 5, 3, 4, 8, 6, 2, 8 };
    uint8_t dpad_index = (pressed(Button::DpadLeft) << 0) | (pressed(Button::DpadRight) << 1) |
                         (pressed(Button::DpadUp) << 2) | (pressed(Button::DpadDown) << 3);
    data[4] = dpad_lut[dpad_index];

    data[5] = (pressed(Button::Capture) << 0) | (pressed(Button::Assistant) << 1) |
              ((l2 > 64) << 2) | ((r2 > 64) << 3) |
              (pressed(Button::Stadia) << 4) | (pressed(Button::Menu) << 5) |
              (pressed(Button::Options) << 6) | (pressed(Button::R3) << 7);

    data[6] = (pressed(Button::L3) << 0) | (pressed(Button::R1) << 1) |
              (pressed(Button::L1) << 2) | (pressed(Button::Y) << 3) |
              (pressed(Button::X) << 4) | (pressed(Button::B) << 5) |
              (pressed(Button::A) << 6);

    data[7] = stick_byte(state.sticks[(int) Stick::Left][0]);
    data[8] = stick_byte(state.sticks[(int) Stick::Left][1]);
    data[9] = stick_byte(state.sticks[(int) Stick::Right][0]);
    data[10] = stick_byte(state.sticks[(int) Stick::Right][1]);

    // analog triggers
    data[11] = l2;
    data[12] = r2;

    data[13] = 0;  // extra buttons

    return STADIA_PACKET_SIZE;
}
