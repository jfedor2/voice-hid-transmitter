// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "notation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

const char* const DIR_NAMES[DIR_COUNT + 1] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW", "C" };

// Canonical button names in the notation, indexed by Button.
static const char* const NOTATION_BUTTONS[BUTTON_COUNT] = {
    "A", "B", "X", "Y",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "UP", "DOWN", "LEFT", "RIGHT",
    "OPTIONS", "MENU", "STADIA", "CAPTURE", "ASSISTANT",
};

struct Alias {
    const char* name;
    int value;
};

static const Alias BUTTON_ALIASES[] = {
    { "LB", (int) Button::L1 }, { "RB", (int) Button::R1 },
    { "LT", (int) Button::L2 }, { "RT", (int) Button::R2 },
    { "ZL", (int) Button::L2 }, { "ZR", (int) Button::R2 },
    { "LSB", (int) Button::L3 }, { "RSB", (int) Button::R3 },
    { "DUP", (int) Button::DpadUp }, { "DDOWN", (int) Button::DpadDown },
    { "DLEFT", (int) Button::DpadLeft }, { "DRIGHT", (int) Button::DpadRight },
    { "DPAD_UP", (int) Button::DpadUp }, { "DPAD_DOWN", (int) Button::DpadDown },
    { "DPAD_LEFT", (int) Button::DpadLeft }, { "DPAD_RIGHT", (int) Button::DpadRight },
    { "START", (int) Button::Menu }, { "SELECT", (int) Button::Options }, { "BACK", (int) Button::Options },
    { "VIEW", (int) Button::Options }, { "HOME", (int) Button::Stadia }, { "GUIDE", (int) Button::Stadia },
    { "SHARE", (int) Button::Capture }, { "GOOGLE", (int) Button::Assistant },
};

static const Alias DIR_ALIASES[] = {
    { "NORTH", 0 }, { "U", 0 }, { "UP", 0 },
    { "NORTHEAST", 1 }, { "UPRIGHT", 1 }, { "UR", 1 },
    { "EAST", 2 }, { "RIGHT", 2 },
    { "SOUTHEAST", 3 }, { "DOWNRIGHT", 3 }, { "DR", 3 },
    { "SOUTH", 4 }, { "D", 4 }, { "DOWN", 4 },
    { "SOUTHWEST", 5 }, { "DOWNLEFT", 5 }, { "DL", 5 },
    { "WEST", 6 }, { "LEFT", 6 },
    { "NORTHWEST", 7 }, { "UPLEFT", 7 }, { "UL", 7 },
    { "CENTER", 8 }, { "CENTRE", 8 }, { "NEUTRAL", 8 }, { "MIDDLE", 8 },
};

static void dir_vector(int dir, float& x, float& y) {
    // Diagonals use 0.71 so they stay on a round gate. y: -1 = up.
    static const float v[DIR_COUNT + 1][2] = {
        { 0, -1 }, { 0.7071f, -0.7071f }, { 1, 0 }, { 0.7071f, 0.7071f },
        { 0, 1 }, { -0.7071f, 0.7071f }, { -1, 0 }, { -0.7071f, -0.7071f }, { 0, 0 },
    };
    x = v[dir][0];
    y = v[dir][1];
}

Target button_target(Button b) {
    Target t;
    t.kind = TargetKind::Button;
    t.index = (int) b;
    return t;
}

Target stick_target(Stick s, int dir, float mag) {
    Target t;
    t.kind = TargetKind::Stick;
    t.index = (int) s;
    t.has_value = true;
    t.dir = dir;
    t.mag = dir == DIR_CENTER ? 1.0f : mag;
    dir_vector(dir, t.x, t.y);
    t.x *= t.mag;
    t.y *= t.mag;
    return t;
}

Target stick_release_target(Stick s) {
    Target t;
    t.kind = TargetKind::Stick;
    t.index = (int) s;
    return t;
}

Target trigger_target(Trigger tr, float value) {
    Target t;
    t.kind = TargetKind::Trigger;
    t.index = (int) tr;
    t.has_value = true;
    t.value = value;
    return t;
}

bool Program::uses_while() const {
    return std::any_of(steps.begin(), steps.end(), [](const Step& s) { return s.verb == Verb::While; });
}

namespace {

enum class Tok {
    Ident,
    Number,
    Plus,
    Equals,
    Star,
    LParen,
    RParen,
    Comma,
    Semicolon,
    End,
    Bad,
};

struct Token {
    Tok type = Tok::End;
    std::string text;  // Ident: uppercased; Number: the unit suffix, uppercased
    double number = 0;
    size_t pos = 0;
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) { next(); }

    bool program(Program& out) {
        out.steps.clear();
        while (true) {
            while (tok_.type == Tok::Semicolon) {
                next();
            }
            if (tok_.type == Tok::End) {
                break;
            }
            Step step;
            if (!parse_step(step)) {
                return false;
            }
            out.steps.push_back(std::move(step));
            if (tok_.type != Tok::Semicolon && tok_.type != Tok::End) {
                return fail("expected ';' between steps");
            }
        }
        if (out.steps.empty()) {
            return fail("empty action");
        }
        return true;
    }

    std::string error;

private:
    bool fail(const std::string& message) {
        if (error.empty()) {
            std::string near;
            if (tok_.type == Tok::End) {
                near = "at the end";
            } else {
                size_t end = std::min(text_.size(), tok_.pos + 12);
                near = "near \"" + text_.substr(tok_.pos, end - tok_.pos) + "\"";
            }
            error = message + " (" + near + ")";
        }
        return false;
    }

    void next() {
        const size_t n = text_.size();
        while (pos_ < n && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r')) {
            pos_++;
        }
        tok_ = Token();
        tok_.pos = pos_;
        if (pos_ >= n) {
            tok_.type = Tok::End;
            return;
        }
        const char c = text_[pos_];
        auto is_ident_char = [](char ch) { return std::isalnum((unsigned char) ch) || ch == '_'; };
        if (std::isalpha((unsigned char) c)) {
            tok_.type = Tok::Ident;
            while (pos_ < n && is_ident_char(text_[pos_])) {
                tok_.text += (char) std::toupper((unsigned char) text_[pos_++]);
            }
            return;
        }
        if (std::isdigit((unsigned char) c) || c == '.' ||
            (c == '-' && pos_ + 1 < n && (std::isdigit((unsigned char) text_[pos_ + 1]) || text_[pos_ + 1] == '.'))) {
            size_t start = pos_++;
            while (pos_ < n && (std::isdigit((unsigned char) text_[pos_]) || text_[pos_] == '.')) {
                pos_++;
            }
            std::string num = text_.substr(start, pos_ - start);
            char* end = nullptr;
            tok_.number = std::strtod(num.c_str(), &end);
            if (end != num.c_str() + num.size() || num == "-" || num == "." || num == "-.") {
                tok_.type = Tok::Bad;
                return;
            }
            tok_.type = Tok::Number;
            // A unit may follow, with or without a space ("200ms", "2 s").
            size_t p = pos_;
            while (p < n && text_[p] == ' ') {
                p++;
            }
            std::string unit;
            size_t q = p;
            while (q < n && std::isalpha((unsigned char) text_[q])) {
                unit += (char) std::toupper((unsigned char) text_[q++]);
            }
            if (unit == "MS" || unit == "MSEC" || unit == "MILLISECOND" || unit == "MILLISECONDS" || unit == "S" ||
                unit == "SEC" || unit == "SECS" || unit == "SECOND" || unit == "SECONDS") {
                tok_.text = unit;
                pos_ = q;
            }
            return;
        }
        pos_++;
        switch (c) {
            case '+': tok_.type = Tok::Plus; break;
            case '=': tok_.type = Tok::Equals; break;
            case '*': tok_.type = Tok::Star; break;
            case '(': tok_.type = Tok::LParen; break;
            case ')': tok_.type = Tok::RParen; break;
            case ',': tok_.type = Tok::Comma; break;
            case ';':
            case '\n': tok_.type = Tok::Semicolon; break;
            default: tok_.type = Tok::Bad; break;
        }
    }

    bool parse_step(Step& step) {
        if (tok_.type != Tok::Ident) {
            return fail("expected press, hold, release, toggle, while or wait");
        }
        const std::string verb = tok_.text;
        if (verb == "PRESS" || verb == "TAP" || verb == "PUSH") {
            step.verb = Verb::Press;
        } else if (verb == "HOLD") {
            step.verb = Verb::Hold;
        } else if (verb == "RELEASE") {
            step.verb = Verb::Release;
        } else if (verb == "TOGGLE") {
            step.verb = Verb::Toggle;
        } else if (verb == "WHILE") {
            step.verb = Verb::While;
        } else if (verb == "WAIT" || verb == "PAUSE" || verb == "DELAY") {
            step.verb = Verb::Wait;
        } else {
            return fail("unknown step \"" + verb + "\"");
        }
        next();

        if (step.verb == Verb::Wait) {
            return parse_duration(step.duration_ms);
        }
        if (step.verb == Verb::Release && tok_.type == Tok::Ident && (tok_.text == "ALL" || tok_.text == "EVERYTHING")) {
            next();
            step.verb = Verb::ReleaseAll;
            return true;
        }
        if (!parse_targets(step)) {
            return false;
        }
        if (step.verb == Verb::Press) {
            if (tok_.type == Tok::Ident && tok_.text == "FOR") {
                next();
                return parse_duration(step.duration_ms);
            }
            if (tok_.type == Tok::Number) {
                return parse_duration(step.duration_ms);
            }
        }
        return true;
    }

    bool parse_duration(int& out_ms) {
        if (tok_.type != Tok::Number) {
            return fail("expected a duration like 200ms or 1.5s");
        }
        if (tok_.text.empty()) {
            return fail("a duration needs a unit, like 200ms or 2s");
        }
        double ms = tok_.number;
        if (tok_.text[0] == 'S') {
            ms *= 1000.0;
        }
        if (ms < 1 || ms > 600000) {
            return fail("duration must be between 1ms and 10 minutes");
        }
        out_ms = (int) std::lround(ms);
        next();
        return true;
    }

    bool parse_targets(Step& step) {
        while (true) {
            Target t;
            if (!parse_target(step.verb, t)) {
                return false;
            }
            step.targets.push_back(t);
            if (tok_.type == Tok::Plus || (tok_.type == Tok::Ident && tok_.text == "AND")) {
                next();
                continue;
            }
            return true;
        }
    }

    // `what` is e.g. "a trigger value", reported with the allowed range.
    bool parse_number(float& out, float lo, float hi, const char* what) {
        char range[96];
        snprintf(range, sizeof(range), "%s (%g to %g)", what, lo, hi);
        if (tok_.type != Tok::Number || !tok_.text.empty()) {
            return fail(std::string("expected ") + range);
        }
        if (tok_.number < lo || tok_.number > hi) {
            return fail(std::string("out of range: ") + range);
        }
        out = (float) tok_.number;
        next();
        return true;
    }

    bool parse_target(Verb verb, Target& t) {
        if (tok_.type != Tok::Ident) {
            return fail("expected a button, stick or trigger");
        }
        const std::string name = tok_.text;
        const bool needs_value = verb != Verb::Release;

        int stick = -1;
        if (name == "LS" || name == "LSTICK" || name == "LEFTSTICK" || name == "LEFT_STICK") {
            stick = (int) Stick::Left;
        } else if (name == "RS" || name == "RSTICK" || name == "RIGHTSTICK" || name == "RIGHT_STICK") {
            stick = (int) Stick::Right;
        }
        if (stick >= 0) {
            next();
            t.kind = TargetKind::Stick;
            t.index = stick;
            if (tok_.type != Tok::Equals) {
                if (needs_value) {
                    return fail("expected a stick position, like LS=NE");
                }
                return true;
            }
            next();
            t.has_value = true;
            return parse_stick_position(t);
        }

        int trigger = -1;
        if (name == "L2" || name == "LT" || name == "ZL") {
            trigger = (int) Trigger::L2;
        } else if (name == "R2" || name == "RT" || name == "ZR") {
            trigger = (int) Trigger::R2;
        }
        int button = -1;
        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (name == NOTATION_BUTTONS[i]) {
                button = i;
            }
        }
        for (const Alias& a : BUTTON_ALIASES) {
            if (name == a.name) {
                button = a.value;
            }
        }
        next();
        if (trigger >= 0 && tok_.type == Tok::Equals) {
            next();
            t.kind = TargetKind::Trigger;
            t.index = trigger;
            t.has_value = true;
            return parse_number(t.value, 0.0f, 1.0f, "a trigger value");
        }
        if (button < 0) {
            return fail("unknown button \"" + name + "\"");
        }
        t.kind = TargetKind::Button;
        t.index = button;
        return true;
    }

    bool parse_stick_position(Target& t) {
        if (tok_.type == Tok::LParen) {
            next();
            float x, y;
            if (!parse_number(x, -1.0f, 1.0f, "an X value")) {
                return false;
            }
            if (tok_.type != Tok::Comma) {
                return fail("expected ','");
            }
            next();
            if (!parse_number(y, -1.0f, 1.0f, "a Y value")) {
                return false;
            }
            if (tok_.type != Tok::RParen) {
                return fail("expected ')'");
            }
            next();
            t.dir = DIR_EXPLICIT;
            t.x = x;
            t.y = -y;  // the notation has +y = up
            return true;
        }
        if (tok_.type != Tok::Ident) {
            return fail("expected a direction (N, NE, E, SE, S, SW, W, NW, C) or (x,y)");
        }
        int dir = -1;
        for (int i = 0; i <= DIR_COUNT; i++) {
            if (tok_.text == DIR_NAMES[i]) {
                dir = i;
            }
        }
        for (const Alias& a : DIR_ALIASES) {
            if (tok_.text == a.name) {
                dir = a.value;
            }
        }
        if (dir < 0) {
            return fail("unknown direction \"" + tok_.text + "\"");
        }
        next();
        t.dir = dir;
        t.mag = 1.0f;
        if (tok_.type == Tok::Star) {
            next();
            if (!parse_number(t.mag, 0.0f, 1.0f, "a deflection")) {
                return false;
            }
        }
        dir_vector(dir, t.x, t.y);
        t.x *= t.mag;
        t.y *= t.mag;
        return true;
    }

    const std::string& text_;
    size_t pos_ = 0;
    Token tok_;
};

std::string format_number(float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

std::string format_duration(int ms) {
    if (ms % 1000 == 0) {
        return std::to_string(ms / 1000) + "s";
    }
    return std::to_string(ms) + "ms";
}

std::string format_target(const Target& t) {
    switch (t.kind) {
        case TargetKind::Button:
            return NOTATION_BUTTONS[t.index];
        case TargetKind::Stick: {
            std::string s = STICK_SHORT_NAMES[t.index];
            if (!t.has_value) {
                return s;
            }
            if (t.dir == DIR_EXPLICIT) {
                return s + "=(" + format_number(t.x) + "," + format_number(-t.y) + ")";
            }
            s += "=";
            s += DIR_NAMES[t.dir];
            if (t.dir != DIR_CENTER && t.mag != 1.0f) {
                s += "*" + format_number(t.mag);
            }
            return s;
        }
        case TargetKind::Trigger:
            if (!t.has_value) {
                return TRIGGER_NAMES[t.index];
            }
            return std::string(TRIGGER_NAMES[t.index]) + "=" + format_number(t.value);
    }
    return "";
}

}  // namespace

bool parse_program(const std::string& text, Program& out, std::string& error) {
    Parser p(text);
    bool ok = p.program(out);
    error = ok ? std::string() : p.error;
    if (!ok) {
        out.steps.clear();
    }
    return ok;
}

std::string format_program(const Program& program) {
    static const char* const verbs[] = { "press", "hold", "release", "toggle", "while", "wait", "release all" };
    std::string out;
    for (const Step& step : program.steps) {
        if (!out.empty()) {
            out += "; ";
        }
        out += verbs[(int) step.verb];
        if (step.verb == Verb::Wait) {
            out += " " + format_duration(step.duration_ms);
            continue;
        }
        for (size_t i = 0; i < step.targets.size(); i++) {
            out += i == 0 ? " " : "+";
            out += format_target(step.targets[i]);
        }
        if (step.verb == Verb::Press && step.duration_ms > 0) {
            out += " " + format_duration(step.duration_ms);
        }
    }
    return out;
}
