// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <winsock2.h>
#include <windows.h>

#include <d3d11.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"

#include "audio.h"
#include "config.h"
#include "engine.h"
#include "notation.h"
#include "embedding.h"
#include "recognizer.h"
#include "sound_vectors.h"
#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static const ImVec4 COLOR_ERROR(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 COLOR_OK(0.45f, 0.9f, 0.55f, 1.0f);
static const ImVec4 COLOR_WARN(1.0f, 0.8f, 0.35f, 1.0f);

// A detection, for the activity log.
struct LogEntry {
    ULONGLONG time = 0;
    uint32_t action_id = 0;
    TriggerType type = TriggerType::Fire;
    float score = 0.0f;
    bool sent = false;
};

// An action's notation, parsed.
struct Parsed {
    std::string text;
    bool ok = false;
    Program program;
    std::string canonical;
    std::string error;
};

// Application state. Owned by the UI thread except where noted.
struct App {
    Config config;
    std::wstring config_path;
    uint32_t next_action_id = 1;
    int selected = -1;  // index into config.actions
    bool running = false;

    uint32_t recording_action = 0;  // 0 = not recording
    // Samples recorded since Record was clicked, with when their loud part was.
    struct SessionClip {
        uint64_t sample_id;
        uint64_t start_ms;
        uint64_t end_ms;
    };
    std::vector<SessionClip> session;
    int session_dropped = 0;
    std::map<uint32_t, Parsed> parsed;
    std::vector<AudioDevice> devices;
    std::vector<uint8_t> play_buffer;  // WAV being played by PlaySound

    bool config_dirty = false;
    ULONGLONG config_dirty_since = 0;
    HWND main_hwnd = nullptr;

    ActionEngine engine;
    Recognizer recognizer;
    AudioCapture audio;

    // Written on the audio thread.
    std::mutex log_mutex;
    std::deque<LogEntry> log;
    std::map<uint32_t, ULONGLONG> last_triggered;
    std::atomic<bool> sending{ false };

    void mark_dirty() {
        if (!config_dirty) {
            config_dirty_since = GetTickCount64();
        }
        config_dirty = true;
    }

    Action* find(uint32_t id) {
        for (Action& a : config.actions) {
            if (a.id == id) {
                return &a;
            }
        }
        return nullptr;
    }

    const Parsed& parse(const Action& a) {
        Parsed& p = parsed[a.id];
        if (p.text != a.notation || (p.text.empty() && p.error.empty())) {
            p.text = a.notation;
            p.ok = parse_program(a.notation, p.program, p.error);
            p.canonical = p.ok ? format_program(p.program) : std::string();
            if (a.notation.find_first_not_of(" \t\r\n") == std::string::npos) {
                p.error = "no action yet";
            }
        }
        return p;
    }

    // Continuous if the action holds something while the sound lasts.
    SoundMode sound_mode(const Action& a) {
        if (a.ignore) {
            return SoundMode::Ignore;
        }
        const Parsed& p = parse(a);
        return p.ok && p.program.uses_while() ? SoundMode::Continuous : SoundMode::Whole;
    }

    // Call after any change to the actions.
    void actions_changed() {
        std::map<uint32_t, Program> programs;
        std::vector<SoundSpec> sounds;
        for (const Action& a : config.actions) {
            if (!a.enabled) {
                continue;
            }
            const Parsed& p = parse(a);
            if (p.ok && !a.ignore) {
                programs[a.id] = p.program;
            }
            SoundSpec s;
            s.action_id = a.id;
            s.mode = sound_mode(a);
            s.threshold = a.margin;
            for (const Sample& sample : a.samples) {
                s.samples.push_back({ sample.id, sample.audio });
            }
            sounds.push_back(std::move(s));
        }
        engine.set_actions(programs);
        recognizer.set_sounds(sounds);
        mark_dirty();
    }
};

static App g_app;

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static bool g_track_placement = false;  // set once the saved placement has been applied

static void capture_window_placement() {
    if (!g_track_placement) {
        return;
    }
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g_app.main_hwnd, &wp)) {
        return;
    }
    WindowRect w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                  (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (!(w == g_app.config.window)) {
        g_app.config.window = w;
        g_app.mark_dirty();
    }
}

static void save_now() {
    save_config(g_app.config_path, g_app.config);
    g_app.config_dirty = false;
}

static void push_button_colors(ImVec4 base) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x * 1.2f, base.y * 1.2f, base.z * 1.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 1.4f, base.y * 1.4f, base.z * 1.4f, 1));
}

static std::string action_label(const Action& a) {
    return a.name.empty() ? "(unnamed)" : a.name;
}

static std::string action_label(uint32_t id) {
    const Action* a = g_app.find(id);
    return a ? action_label(*a) : "(deleted)";
}

// ---------------------------------------------------------------------------
// Actions

static void select_action(int index) {
    if (g_app.recording_action != 0 &&
        (index < 0 || index >= (int) g_app.config.actions.size() ||
            g_app.config.actions[index].id != g_app.recording_action)) {
        g_app.recognizer.stop_recording();
        g_app.recording_action = 0;
    }
    g_app.selected = index;
}

static void add_action() {
    Action a;
    a.id = g_app.next_action_id++;
    a.name = "Action " + std::to_string(g_app.config.actions.size() + 1);
    g_app.config.actions.push_back(a);
    select_action((int) g_app.config.actions.size() - 1);
    g_app.actions_changed();
}

static void delete_action(int index) {
    App& app = g_app;
    if (index < 0 || index >= (int) app.config.actions.size()) {
        return;
    }
    const Action& a = app.config.actions[index];
    if (app.recording_action == a.id) {
        app.recognizer.stop_recording();
        app.recording_action = 0;
    }
    for (const Sample& s : a.samples) {
        DeleteFileW((samples_dir() + L"\\" + from_utf8(s.file)).c_str());
    }
    app.parsed.erase(a.id);
    app.config.actions.erase(app.config.actions.begin() + index);
    if (app.selected == index) {
        app.selected = -1;
    } else if (app.selected > index) {
        app.selected--;
    }
    app.actions_changed();
}

static void delete_sample(Action& a, size_t index) {
    DeleteFileW((samples_dir() + L"\\" + from_utf8(a.samples[index].file)).c_str());
    a.samples.erase(a.samples.begin() + index);
    g_app.actions_changed();
}

static void play_audio(const Audio& audio) {
    PlaySoundW(nullptr, nullptr, 0);  // stop whatever is playing before freeing its buffer
    g_app.play_buffer = wav_bytes(audio);
    PlaySoundW((LPCWSTR) g_app.play_buffer.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// Moves newly recorded sounds into the action being recorded.
// Leaves out noises recorded along with the intended sound, like a breath or
// a mouth noise right before or after it. Such a noise is much shorter than
// the session's typical sound and right next to a longer one. Relative to the
// session, so a session of short sounds (clicks) keeps them all.
static constexpr float NOISE_LENGTH_RATIO = 0.3f;
static constexpr uint64_t NOISE_GAP_MS = 1000;

static void drop_noises(Action& a) {
    App& app = g_app;
    if (app.session.size() < 2) {
        return;
    }
    std::vector<uint64_t> lengths;
    for (const auto& c : app.session) {
        lengths.push_back(c.end_ms - c.start_ms);
    }
    std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
    const float limit = lengths[lengths.size() / 2] * NOISE_LENGTH_RATIO;
    auto is_short = [limit](const App::SessionClip& c) { return c.end_ms - c.start_ms < limit; };

    std::set<uint64_t> drop;
    for (const auto& c : app.session) {
        if (!is_short(c)) {
            continue;
        }
        for (const auto& d : app.session) {
            const uint64_t gap = d.start_ms >= c.end_ms ? d.start_ms - c.end_ms
                                 : c.start_ms >= d.end_ms ? c.start_ms - d.end_ms
                                                          : 0;
            if (!is_short(d) && gap < NOISE_GAP_MS) {
                drop.insert(c.sample_id);
                break;
            }
        }
    }
    for (uint64_t id : drop) {
        for (size_t i = 0; i < a.samples.size(); i++) {
            if (a.samples[i].id == id) {
                DeleteFileW((samples_dir() + L"\\" + from_utf8(a.samples[i].file)).c_str());
                a.samples.erase(a.samples.begin() + i);
                break;
            }
        }
    }
    app.session.erase(std::remove_if(app.session.begin(), app.session.end(),
                          [&](const App::SessionClip& c) { return drop.count(c.sample_id) != 0; }),
        app.session.end());
    app.session_dropped += (int) drop.size();
}

static void collect_recordings() {
    App& app = g_app;
    std::vector<RecordedClip> clips = app.recognizer.take_recorded();
    if (clips.empty()) {
        return;
    }
    Action* a = app.find(app.recording_action);
    if (a == nullptr) {
        return;
    }
    for (RecordedClip& clip : clips) {
        Sample s;
        s.id = new_sample_id();
        s.file = std::to_string(a->id) + "_" + std::to_string(s.id) + ".wav";
        if (!write_wav(samples_dir() + L"\\" + from_utf8(s.file), clip.audio)) {
            continue;
        }
        s.audio = std::make_shared<Audio>(std::move(clip.audio));
        a->samples.push_back(s);
        app.session.push_back({ s.id, clip.loud_start_ms, clip.loud_end_ms });
    }
    drop_noises(*a);
    app.actions_changed();
}

// ---------------------------------------------------------------------------
// Widgets

static void draw_stick_indicator(Stick stick, const float (&value)[2]) {
    const float em = ImGui::GetFontSize();
    const float radius = em * 1.3f;
    const ImVec2 size(radius * 2, radius * 2);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + radius, p.y + radius);
    const Color3& col = STICK_COLORS[(int) stick];
    const bool centered = stick_byte(value[0]) == 128 && stick_byte(value[1]) == 128;
    dl->AddCircleFilled(c, radius, IM_COL32(40, 42, 48, 255));
    dl->AddCircle(c, radius, ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 1)), 0, 1.5f);
    dl->AddLine(ImVec2(c.x - radius, c.y), ImVec2(c.x + radius, c.y), IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(c.x, c.y - radius), ImVec2(c.x, c.y + radius), IM_COL32(255, 255, 255, 30));
    const ImVec2 dot(c.x + value[0] * (radius - 3), c.y + value[1] * (radius - 3));
    dl->AddCircleFilled(dot, em * 0.22f,
        centered ? IM_COL32(120, 120, 130, 255)
                 : ImGui::GetColorU32(ImVec4(col.r + (1 - col.r) * 0.4f, col.g + (1 - col.g) * 0.4f, col.b + (1 - col.b) * 0.4f, 1)));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(STICK_NAMES[(int) stick]);
    ImGui::TextDisabled("%+.2f, %+.2f", value[0], -value[1]);
    ImGui::EndGroup();
    ImGui::EndGroup();
}

static void draw_output() {
    const GamepadState state = g_app.engine.current_state();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Effective trigger travel: a pressed L2/R2 button counts as fully pressed.
    float trigger_travel[TRIGGER_COUNT];
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        trigger_travel[t] = (state.buttons & button_mask(trigger_button((Trigger) t))) ? 1.0f : state.triggers[t];
    }
    float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool on = (state.buttons & (1u << i)) != 0;
        if (i == (int) Button::L2 || i == (int) Button::R2) {
            // Same threshold as the digital bit in the report.
            on = trigger_byte(trigger_travel[i == (int) Button::L2 ? 0 : 1]) > 64;
        }
        const char* name = BUTTON_NAMES[i];
        float w = ImGui::CalcTextSize(name).x + style.FramePadding.x * 2;
        if (i > 0) {
            float next = ImGui::GetItemRectMax().x + style.ItemSpacing.x + w;
            if (next <= right_edge) {
                ImGui::SameLine();
            }
        }
        const Color3& c = BUTTON_COLORS[i];
        ImVec4 col = on ? ImVec4(c.r, c.g, c.b, 1) : ImVec4(0.18f, 0.19f, 0.22f, 1);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.55f, 1));
        ImGui::PushID(i);
        ImGui::Button(name);
        ImGui::PopID();
        ImGui::PopStyleColor(4);
    }
    const float em = ImGui::GetFontSize();
    const float slot_w = em * 2.6f + style.ItemSpacing.x +
                         std::max(ImGui::CalcTextSize(STICK_NAMES[(int) Stick::Right]).x,
                             ImGui::CalcTextSize("-0.00, -0.00").x * 1.15f);
    const float x0 = ImGui::GetCursorPosX();
    draw_stick_indicator(Stick::Left, state.sticks[(int) Stick::Left]);
    ImGui::SameLine(x0 + slot_w + em);
    draw_stick_indicator(Stick::Right, state.sticks[(int) Stick::Right]);
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        const Color3& c = BUTTON_COLORS[(int) trigger_button((Trigger) t)];
        char overlay[16];
        snprintf(overlay, sizeof(overlay), "%.2f", trigger_travel[t]);
        if (t > 0) {
            ImGui::SameLine(0, em);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TRIGGER_NAMES[t]);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(c.r, c.g, c.b, 1));
        ImGui::ProgressBar(trigger_travel[t], ImVec2(em * 6, em * 1.1f), overlay);
        ImGui::PopStyleColor();
    }
}

// Horizontal meter of a match score from lo to hi, with the threshold marked.
static void draw_score_bar(float score, float threshold, float lo, float hi, bool flash, bool active, float width) {
    const float em = ImGui::GetFontSize();
    const ImVec2 size(width, em * 0.9f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto frac = [&](float v) { return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 42, 48, 255), 3);
    const float fill = frac(score) * size.x;
    const bool over = score >= threshold;
    ImU32 col = active ? IM_COL32(80, 200, 255, 255) : over ? IM_COL32(70, 190, 100, 255) : IM_COL32(95, 100, 120, 255);
    if (fill > 0) {
        dl->AddRectFilled(p, ImVec2(p.x + fill, p.y + size.y), col, 3);
    }
    const float tx = p.x + frac(threshold) * size.x;
    dl->AddLine(ImVec2(tx, p.y - 1), ImVec2(tx, p.y + size.y + 1), IM_COL32(255, 255, 255, 170), 1.5f);
    if (flash) {
        dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + size.x + 1, p.y + size.y + 1), IM_COL32(120, 255, 150, 255), 3.0f, ImDrawFlags_None, 2.0f);
    }
}

// Microphone level with the background level and the gate threshold marked.
static void draw_level_meter(const RecognizerStatus& st, float width) {
    const float em = ImGui::GetFontSize();
    const ImVec2 size(width, em * 0.9f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lo = -90.0f, hi = 0.0f;
    auto x_of = [&](float db) { return p.x + std::clamp((db - lo) / (hi - lo), 0.0f, 1.0f) * size.x; };
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 42, 48, 255), 3);
    dl->AddRectFilled(p, ImVec2(x_of(st.level_db), p.y + size.y),
        st.in_sound ? IM_COL32(70, 190, 100, 255) : IM_COL32(95, 100, 120, 255), 3);
    dl->AddLine(ImVec2(x_of(st.floor_db), p.y), ImVec2(x_of(st.floor_db), p.y + size.y), IM_COL32(120, 160, 255, 220), 1.5f);
    dl->AddLine(ImVec2(x_of(st.threshold_db), p.y - 2), ImVec2(x_of(st.threshold_db), p.y + size.y + 2), IM_COL32(255, 255, 255, 220), 2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Level %.0f dB\nBackground %.0f dB (blue)\nSounds start above %.0f dB (white)", st.level_db,
            st.floor_db, st.threshold_db);
    }
}

static void notation_help() {
    ImGui::TextUnformatted("Steps, separated by \";\", run in order:");
    ImGui::BulletText("press A / press A+B 200ms: press briefly (100 ms) or for a time, then release");
    ImGui::BulletText("hold X: press and keep held");
    ImGui::BulletText("release X / release all: let go");
    ImGui::BulletText("toggle L3: hold if not held, otherwise release");
    ImGui::BulletText("while A: hold for as long as the sound lasts, then go on (continuous sounds)");
    ImGui::BulletText("wait 150ms: pause before the next step");
    ImGui::Spacing();
    ImGui::TextUnformatted("Targets:");
    ImGui::BulletText("Buttons: A B X Y L1 R1 L2 R2 L3 R3 UP DOWN LEFT RIGHT OPTIONS MENU STADIA CAPTURE ASSISTANT");
    ImGui::BulletText("Sticks: LS=NE, RS=N*0.5 (N NE E SE S SW W NW, C = center), LS=(0.3,-1) with +y = up");
    ImGui::BulletText("Analog triggers: L2=0.5, R2=0.2 (plain L2 / R2 = fully pressed)");
    ImGui::Spacing();
    ImGui::TextDisabled("A new stick or trigger position overrides the previous one until it ends.");
    ImGui::TextDisabled("Example: press A; wait 100ms; press A        (double tap)");
}

// ---------------------------------------------------------------------------
// Panels

static void draw_top_bar() {
    App& app = g_app;
    Config& cfg = app.config;
    const float em = ImGui::GetFontSize();

    const ImVec2 big(em * 9, ImGui::GetFrameHeight() * 1.7f);
    push_button_colors(app.running ? ImVec4(0.13f, 0.52f, 0.22f, 1) : ImVec4(0.45f, 0.17f, 0.17f, 1));
    if (ImGui::Button(app.running ? "RUNNING" : "STOPPED", big)) {
        app.running = !app.running;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(app.running ? "Click to stop: releases everything and stops sending inputs.\n"
                                        "Sounds are still recognized (see the activity log)."
                                      : "Click to start sending inputs to the receiver.");

    ImGui::SameLine(0, em * 1.5f);
    ImGui::BeginGroup();
    // Receiver
    static std::string ip_buffer = cfg.receiver_ip;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Receiver IP");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::InputTextWithHint("##ip", "e.g. 192.168.1.50", &ip_buffer, ImGuiInputTextFlags_CharsNoBlank)) {
        cfg.receiver_ip = ip_buffer;
        app.engine.set_receiver(cfg.receiver_ip);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("IP address of the HID Forwarder receiver (Pico W). Inputs are sent as UDP to port 42734.");
    ImGui::SameLine();
    if (cfg.receiver_ip.empty()) {
        ImGui::TextDisabled("not set");
    } else if (!app.engine.address_valid()) {
        ImGui::TextColored(COLOR_ERROR, "invalid IPv4 address");
    } else if (app.engine.send_error()) {
        ImGui::TextColored(COLOR_ERROR, "send failed");
    } else if (app.running) {
        ImGui::TextColored(COLOR_OK, "sending (%llu packets)", (unsigned long long) app.engine.packets_sent());
    } else {
        ImGui::TextDisabled("idle");
    }

    // Microphone
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Microphone");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 16);
    std::string current = "System default";
    for (const AudioDevice& d : app.devices) {
        if (d.id == cfg.mic_id) {
            current = d.name;
        }
    }
    if (!cfg.mic_id.empty() && current == "System default") {
        current = "(unavailable device)";
    }
    if (ImGui::BeginCombo("##mic", current.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            app.devices = list_capture_devices();
        }
        if (ImGui::Selectable("System default", cfg.mic_id.empty())) {
            cfg.mic_id.clear();
            app.audio.set_device(cfg.mic_id);
            app.mark_dirty();
        }
        for (const AudioDevice& d : app.devices) {
            ImGui::PushID(d.id.c_str());
            if (ImGui::Selectable(d.name.c_str(), d.id == cfg.mic_id)) {
                cfg.mic_id = d.id;
                app.audio.set_device(cfg.mic_id);
                app.mark_dirty();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const std::string capturing = app.audio.device_name();
    if (!capturing.empty()) {
        ImGui::SetItemTooltip("Listening to: %s", capturing.c_str());
    }
    ImGui::SameLine();
    const RecognizerStatus st = app.recognizer.status();
    const std::string mic_error = app.audio.error();
    if (!mic_error.empty()) {
        ImGui::TextColored(COLOR_ERROR, "%s", mic_error.c_str());
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - em * 0.9f) / 2);
        draw_level_meter(st, em * 12);
        // Nothing but digital silence since the mic was opened: it's muted or
        // blocked. (A quiet mic can be silent between sounds, so only "never".)
        if (!st.signal_seen && st.stream_seconds > 3.0f) {
            ImGui::SameLine();
            ImGui::TextColored(COLOR_WARN, "No signal. Muted, or blocked in Windows privacy settings?");
        }
    }
    ImGui::EndGroup();
}

// The match bar of whole sounds shows margins from -MARGIN_RANGE to MARGIN_RANGE.
static constexpr float MARGIN_RANGE = 0.15f;

static void draw_actions_table(const RecognizerStatus& st) {
    App& app = g_app;
    const float em = ImGui::GetFontSize();
    const ULONGLONG now = GetTickCount64();
    std::map<uint32_t, ULONGLONG> last_triggered;
    {
        std::lock_guard<std::mutex> lock(app.log_mutex);
        last_triggered = app.last_triggered;
    }
    const std::vector<uint32_t> busy = app.engine.busy_actions();

    if (ImGui::Button("Add action")) {
        add_action();
    }
    ImGui::SameLine();
    if (!speech::embedder().loaded() || !background().loaded()) {
        ImGui::TextColored(COLOR_ERROR, "The sound model couldn't be loaded: words and clicks won't be recognized.");
    } else if (app.recognizer.building()) {
        ImGui::TextColored(COLOR_WARN, "Learning the sounds...");
    } else {
        ImGui::TextDisabled("An action: a sound you make, and what the controller does when it hears it.");
    }

    int delete_index = -1;
    const float table_h = std::max(ImGui::GetFrameHeightWithSpacing() * 4, ImGui::GetContentRegionAvail().y * 0.38f);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("actions", 6, flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, em * 8);
        ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthFixed, em * 7.5f);
        ImGui::TableSetupColumn("Does", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int) app.config.actions.size(); i++) {
            Action& a = app.config.actions[i];
            ImGui::PushID((int) a.id);
            ImGui::TableNextRow();
            const bool is_busy = std::find(busy.begin(), busy.end(), a.id) != busy.end();
            if (is_busy) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(40, 90, 55, 160));
            }

            // The row is selectable; the checkbox, drawn over it, gets its own clicks.
            ImGui::TableNextColumn();
            const ImVec2 cell = ImGui::GetCursorPos();
            if (ImGui::Selectable("##row", app.selected == i,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0, ImGui::GetFrameHeight()))) {
                select_action(i);
            }
            ImGui::SetCursorPos(cell);
            if (ImGui::Checkbox("##enabled", &a.enabled)) {
                app.actions_changed();
            }
            ImGui::SetItemTooltip(a.enabled ? "Enabled. Uncheck to turn the action off without deleting it."
                                            : "Disabled: the action is left out, as if it didn't exist.");
            if (!a.enabled) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action_label(a).c_str());

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const SoundMode mode = app.sound_mode(a);
            if (app.recording_action == a.id) {
                ImGui::TextColored(COLOR_ERROR, "recording...");
            } else if (a.samples.empty()) {
                ImGui::TextColored(COLOR_WARN, "not recorded");
            } else {
                ImGui::Text("%d %s", (int) a.samples.size(),
                    mode == SoundMode::Continuous ? "contin." : mode == SoundMode::Ignore ? "blocker" : "whole");
            }

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const Parsed& p = app.parse(a);
            if (mode == SoundMode::Ignore) {
                ImGui::TextDisabled("(not a command: blocks this sound)");
            } else if (p.ok) {
                ImGui::TextUnformatted(p.canonical.c_str());
            } else {
                ImGui::TextColored(COLOR_WARN, "%s", p.error.c_str());
            }

            ImGui::TableNextColumn();
            auto it = st.scores.find(a.id);
            const float score = it != st.scores.end() ? it->second : -1e9f;  // nothing heard yet
            auto lt = last_triggered.find(a.id);
            const bool flash = lt != last_triggered.end() && now - lt->second < 400;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - em * 0.9f) / 2);
            if (mode != SoundMode::Ignore && a.enabled) {
                draw_score_bar(score, a.margin, -MARGIN_RANGE, MARGIN_RANGE, flash, st.active_continuous == a.id, em * 6.2f);
            }
            if (!a.enabled) {
                ImGui::PopStyleColor();
            }

            ImGui::TableNextColumn();
            if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), 0))) {
                delete_index = i;
            }
            ImGui::SetItemTooltip("Delete action and its recorded sound");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (delete_index >= 0) {
        delete_action(delete_index);
    }
}

// ---------------------------------------------------------------------------
// Step editor: builds the action's program with widgets. The notation text
// stays the stored form; the editor parses it, edits the program and writes
// it back.

static const char* const VERB_LABELS[] = { "press", "hold", "release", "toggle", "while", "wait", "release all" };
static const char* const VERB_HELP[] = {
    "Press, then release after the time. The next step starts when it's released.",
    "Press and keep held, until released by this or another action.",
    "Let go. A stick goes back to center, a trigger to 0.",
    "Hold if not held, otherwise release.",
    "Hold for as long as the sound lasts, then go on (detection becomes continuous).",
    "Pause before the next step.",
    "Let go of everything and stop other running actions.",
};
constexpr int VERB_COUNT = 7;
constexpr int MAX_TARGETS = 4;

static bool verb_has_targets(Verb v) {
    return v != Verb::Wait && v != Verb::ReleaseAll;
}

static bool verb_has_duration(Verb v) {
    return v == Verb::Press || v == Verb::Wait;
}

static std::string target_label(const Target& t) {
    char buf[48];
    switch (t.kind) {
        case TargetKind::Button:
            return BUTTON_NAMES[t.index];
        case TargetKind::Stick: {
            const char* s = STICK_SHORT_NAMES[t.index];
            if (!t.has_value) {
                snprintf(buf, sizeof(buf), "%s", STICK_NAMES[t.index]);
            } else if (t.dir == DIR_CENTER) {
                snprintf(buf, sizeof(buf), "%s center", s);
            } else if (t.dir == DIR_EXPLICIT) {
                snprintf(buf, sizeof(buf), "%s (%.2f, %.2f)", s, t.x, -t.y);
            } else if (t.mag < 0.995f) {
                snprintf(buf, sizeof(buf), "%s %s %d%%", s, DIR_NAMES[t.dir], (int) std::lround(t.mag * 100));
            } else {
                snprintf(buf, sizeof(buf), "%s %s", s, DIR_NAMES[t.dir]);
            }
            return buf;
        }
        case TargetKind::Trigger:
            if (!t.has_value) {
                return TRIGGER_NAMES[t.index];
            }
            snprintf(buf, sizeof(buf), "%s %d%%", TRIGGER_NAMES[t.index], (int) std::lround(t.value * 100));
            return buf;
    }
    return "";
}

static ImVec4 target_color(const Target& t) {
    Color3 c = t.kind == TargetKind::Stick     ? STICK_COLORS[t.index]
               : t.kind == TargetKind::Trigger ? BUTTON_COLORS[(int) trigger_button((Trigger) t.index)]
                                               : BUTTON_COLORS[t.index];
    return ImVec4(c.r * 0.6f, c.g * 0.6f, c.b * 0.6f, 1.0f);
}

// A button that shows as selected (highlighted) or not.
static bool toggle_button(const char* label, bool selected, ImVec2 size = ImVec2(0, 0)) {
    if (selected) {
        push_button_colors(ImVec4(0.20f, 0.45f, 0.80f, 1));
    }
    const bool clicked = ImGui::Button(label, size);
    if (selected) {
        ImGui::PopStyleColor(3);
    }
    return clicked;
}

// 3x3 grid of stick directions. Returns the clicked direction, or -1.
static int direction_pad(int selected_dir) {
    static const int grid[9] = { 7, 0, 1, 6, DIR_CENTER, 2, 5, 4, 3 };
    const float cell = ImGui::GetFrameHeight() * 1.25f;
    int clicked = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    for (int i = 0; i < 9; i++) {
        const int dir = grid[i];
        if (i % 3 != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(i);
        if (toggle_button("##dir", dir == selected_dir, ImVec2(cell, cell))) {
            clicked = dir;
        }
        ImGui::SetItemTooltip("%s", dir == DIR_CENTER ? "Center" : DIR_NAMES[dir]);
        ImGui::PopID();
        // Drawn rather than using arrow glyphs, which Segoe UI only partly has.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImVec2 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2);
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        const float len = cell * 0.28f;
        if (dir == DIR_CENTER) {
            dl->AddCircleFilled(c, cell * 0.09f, col);
        } else {
            const Target t = stick_target(Stick::Left, dir, 1.0f);
            const float m = std::sqrt(t.x * t.x + t.y * t.y);
            const float dx = t.x / m, dy = t.y / m;
            const ImVec2 tip(c.x + dx * len, c.y + dy * len);
            const float th = std::max(1.5f, cell * 0.06f);
            const float hl = len * 0.7f;
            dl->AddLine(ImVec2(c.x - dx * len, c.y - dy * len), tip, col, th);
            dl->AddLine(tip, ImVec2(tip.x - hl * (dx * 0.707f - dy * 0.707f), tip.y - hl * (dy * 0.707f + dx * 0.707f)), col, th);
            dl->AddLine(tip, ImVec2(tip.x - hl * (dx * 0.707f + dy * 0.707f), tip.y - hl * (dy * 0.707f - dx * 0.707f)), col, th);
        }
    }
    ImGui::PopStyleVar();
    return clicked;
}

// Contents of the popup that picks a target. Returns true when `t` was set.
// Clicking a target closes the popup; the sliders change the current
// stick deflection or trigger value in place.
static bool target_picker(Verb verb, Target& t) {
    static float deflection = 100.0f;    // percent
    static float trigger_value = 50.0f;  // percent
    if (ImGui::IsWindowAppearing()) {
        if (t.kind == TargetKind::Stick && t.has_value && t.dir >= 0 && t.dir != DIR_CENTER) {
            deflection = t.mag * 100.0f;
        }
        if (t.kind == TargetKind::Trigger && t.has_value) {
            trigger_value = t.value * 100.0f;
        }
    }
    const float em = ImGui::GetFontSize();
    bool picked = false;
    auto pick = [&](const Target& nt) {
        t = nt;
        picked = true;
        ImGui::CloseCurrentPopup();
    };

    ImGui::SeparatorText("Buttons");
    static const Button rows[][5] = {
        { Button::L2, Button::L1, Button::R1, Button::R2, Button::Count },
        { Button::Y, Button::X, Button::B, Button::A, Button::Count },
        { Button::DpadUp, Button::DpadDown, Button::DpadLeft, Button::DpadRight, Button::Count },
        { Button::L3, Button::R3, Button::Count },
        { Button::Options, Button::Menu, Button::Stadia, Button::Capture, Button::Assistant },
    };
    const float bw = em * 4.2f;
    for (const auto& row : rows) {
        for (int i = 0; i < 5 && row[i] != Button::Count; i++) {
            if (i > 0) {
                ImGui::SameLine();
            }
            const Button b = row[i];
            const bool selected = t.kind == TargetKind::Button && t.index == (int) b;
            if (toggle_button(BUTTON_NAMES[(int) b], selected, ImVec2(bw, 0))) {
                pick(button_target(b));
            }
            if (b == Button::DpadUp) {
                ImGui::SetItemTooltip("D-pad");
            }
        }
    }

    ImGui::SeparatorText("Sticks");
    if (verb == Verb::Release) {
        for (int s = 0; s < STICK_COUNT; s++) {
            if (s > 0) {
                ImGui::SameLine();
            }
            const bool selected = t.kind == TargetKind::Stick && t.index == s;
            if (toggle_button(STICK_NAMES[s], selected, ImVec2(bw * 2 + ImGui::GetStyle().ItemSpacing.x, 0))) {
                pick(stick_release_target((Stick) s));
            }
        }
        ImGui::TextDisabled("L2 and R2 above release the trigger too.");
        return picked;
    }
    for (int s = 0; s < STICK_COUNT; s++) {
        if (s > 0) {
            ImGui::SameLine(0, em * 2);
        }
        ImGui::BeginGroup();
        ImGui::TextUnformatted(STICK_NAMES[s]);
        ImGui::PushID(s);
        const bool this_stick = t.kind == TargetKind::Stick && t.index == s && t.has_value;
        const int dir = direction_pad(this_stick ? t.dir : -2);
        if (dir >= 0) {
            pick(stick_target((Stick) s, dir, deflection / 100.0f));
        }
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::SliderFloat("Deflection", &deflection, 10.0f, 100.0f, "%.0f%%")) {
        deflection = std::round(deflection / 5.0f) * 5.0f;
        if (t.kind == TargetKind::Stick && t.has_value && t.dir >= 0 && t.dir != DIR_CENTER) {
            t = stick_target((Stick) t.index, t.dir, deflection / 100.0f);
            picked = true;
        }
    }
    ImGui::SetItemTooltip("How far the stick is pushed. Pick the direction after setting it.");

    ImGui::SeparatorText("Analog triggers, partly pulled");
    ImGui::SetNextItemWidth(em * 10);
    if (ImGui::SliderFloat("Pull", &trigger_value, 5.0f, 100.0f, "%.0f%%")) {
        trigger_value = std::round(trigger_value / 5.0f) * 5.0f;
        if (t.kind == TargetKind::Trigger && t.has_value) {
            t = trigger_target((Trigger) t.index, trigger_value / 100.0f);
            picked = true;
        }
    }
    for (int tr = 0; tr < TRIGGER_COUNT; tr++) {
        ImGui::SameLine();
        char label[32];
        snprintf(label, sizeof(label), "%s %.0f%%", TRIGGER_NAMES[tr], trigger_value);
        const bool selected = t.kind == TargetKind::Trigger && t.index == tr && t.has_value;
        if (toggle_button(label, selected, ImVec2(em * 5, 0))) {
            pick(trigger_target((Trigger) tr, trigger_value / 100.0f));
        }
    }
    return picked;
}

static void change_verb(Step& s, Verb v) {
    s.verb = v;
    if (!verb_has_targets(v)) {
        s.targets.clear();
    } else {
        if (s.targets.empty()) {
            s.targets.push_back(button_target(Button::A));
        }
        for (Target& t : s.targets) {
            if (v == Verb::Release) {
                // Release takes whole controls, not positions.
                if (t.kind == TargetKind::Stick) {
                    t = stick_release_target((Stick) t.index);
                } else if (t.kind == TargetKind::Trigger) {
                    t = button_target(trigger_button((Trigger) t.index));
                }
            } else if (t.kind == TargetKind::Stick && !t.has_value) {
                t = stick_target((Stick) t.index, 0, 1.0f);
            }
        }
    }
    if (!verb_has_duration(v)) {
        s.duration_ms = -1;
    } else if (v == Verb::Wait && s.duration_ms <= 0) {
        s.duration_ms = 100;
    }
}

// Edits a program. Returns true if it changed.
static bool draw_steps_editor(Program& prog) {
    const float em = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    bool changed = false;
    int remove = -1, move_up = -1;
    const float right_edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    bool seen_while = false;

    for (int i = 0; i < (int) prog.steps.size(); i++) {
        Step& s = prog.steps[i];
        // The first "while" step waits for the sound to stop, so by any later
        // one it has stopped already.
        const bool useless_while = s.verb == Verb::While && seen_while;
        seen_while |= s.verb == Verb::While;
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d", i + 1);
        ImGui::SameLine(em * 1.5f);

        ImGui::SetNextItemWidth(em * 6.5f);
        if (ImGui::BeginCombo("##verb", VERB_LABELS[(int) s.verb])) {
            for (int v = 0; v < VERB_COUNT; v++) {
                if (ImGui::Selectable(VERB_LABELS[v], (int) s.verb == v) && (int) s.verb != v) {
                    change_verb(s, (Verb) v);
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", VERB_HELP[v]);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", VERB_HELP[(int) s.verb]);

        if (verb_has_targets(s.verb)) {
            int remove_target = -1;
            for (int j = 0; j < (int) s.targets.size(); j++) {
                ImGui::SameLine();
                ImGui::PushID(j);
                if (j > 0) {
                    ImGui::TextDisabled("+");
                    ImGui::SameLine();
                }
                push_button_colors(target_color(s.targets[j]));
                if (ImGui::Button(target_label(s.targets[j]).c_str())) {
                    ImGui::OpenPopup("target");
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && s.targets.size() > 1) {
                    remove_target = j;
                }
                ImGui::SetItemTooltip(s.targets.size() > 1 ? "Click to change, right-click to remove." : "Click to change.");
                if (ImGui::BeginPopup("target")) {
                    if (target_picker(s.verb, s.targets[j])) {
                        changed = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (remove_target >= 0) {
                s.targets.erase(s.targets.begin() + remove_target);
                changed = true;
            }
            if ((int) s.targets.size() < MAX_TARGETS) {
                ImGui::SameLine();
                if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0))) {
                    ImGui::OpenPopup("add");
                }
                ImGui::SetItemTooltip("Add a target, used at the same time.");
                if (ImGui::BeginPopup("add")) {
                    Target t;
                    t.index = -1;  // nothing selected
                    if (target_picker(s.verb, t) && t.index >= 0) {
                        s.targets.push_back(t);
                        changed = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }

        if (verb_has_duration(s.verb)) {
            ImGui::SameLine(0, em);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.verb == Verb::Press ? "for" : "");
            ImGui::SameLine();
            int ms = s.duration_ms > 0 ? s.duration_ms : DEFAULT_PRESS_MS;
            ImGui::SetNextItemWidth(em * 5.5f);
            if (ImGui::DragInt("##ms", &ms, 5.0f, 10, 60000, "%d ms", ImGuiSliderFlags_AlwaysClamp)) {
                s.duration_ms = ms;
                changed = true;
            }
            ImGui::SetItemTooltip("Drag or double-click to type.");
        }
        if (useless_while) {
            ImGui::SameLine(0, em);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(COLOR_WARN, "does nothing");
            ImGui::SetItemTooltip("The \"while\" step before this one waits for the sound to stop, so by the\n"
                                  "time this step runs, the sound has stopped and there's nothing to hold.");
        }

        // Reorder and delete, at the right edge.
        const float buttons_w = ImGui::GetFrameHeight() * 3 + style.ItemSpacing.x * 2;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right_edge - buttons_w));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            move_up = i;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == (int) prog.steps.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            move_up = i + 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
            remove = i;
        }
        ImGui::SetItemTooltip("Delete step");
        ImGui::PopID();
    }
    if (move_up > 0) {
        std::swap(prog.steps[move_up - 1], prog.steps[move_up]);
        changed = true;
    }
    if (remove >= 0) {
        prog.steps.erase(prog.steps.begin() + remove);
        changed = true;
    }

    if (ImGui::Button("Add step")) {
        Step s;
        // Steps usually follow up on the previous step's target.
        Target t = button_target(Button::A);
        for (auto it = prog.steps.rbegin(); it != prog.steps.rend(); ++it) {
            if (!it->targets.empty()) {
                t = it->targets[0];
                break;
            }
        }
        s.targets.push_back(t);
        change_verb(s, Verb::Press);
        prog.steps.push_back(s);
        changed = true;
    }

    // Presets replace the whole program, keeping its first target.
    ImGui::SameLine(0, em * 1.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Presets:");
    Target first = button_target(Button::A);
    for (const Step& s : prog.steps) {
        if (!s.targets.empty()) {
            first = s.targets[0];
            break;
        }
    }
    if (first.kind == TargetKind::Stick && !first.has_value) {
        first = stick_target((Stick) first.index, 0, 1.0f);
    }
    struct Preset {
        const char* name;
        const char* help;
        std::vector<std::pair<Verb, int>> steps;  // verb, duration
    };
    static const Preset presets[] = {
        { "Tap", "Press briefly.", { { Verb::Press, -1 } } },
        { "Hold", "Press and keep held.", { { Verb::Hold, -1 } } },
        { "Toggle", "Hold on one sound, release on the next.", { { Verb::Toggle, -1 } } },
        { "Double tap", "Press twice.", { { Verb::Press, -1 }, { Verb::Wait, 100 }, { Verb::Press, -1 } } },
        { "While sounding", "Hold for as long as the sound lasts.", { { Verb::While, -1 } } },
    };
    for (const Preset& p : presets) {
        ImGui::SameLine();
        if (ImGui::SmallButton(p.name)) {
            prog.steps.clear();
            for (const auto& [verb, ms] : p.steps) {
                Step s;
                s.verb = verb;
                if (verb_has_targets(verb)) {
                    s.targets.push_back(first);
                }
                s.duration_ms = ms;
                prog.steps.push_back(s);
            }
            changed = true;
        }
        ImGui::SetItemTooltip("%s Replaces the steps, keeping the first target (%s).", p.help, target_label(first).c_str());
    }
    return changed;
}

static void draw_action_details() {
    App& app = g_app;
    const float em = ImGui::GetFontSize();
    if (app.selected < 0 || app.selected >= (int) app.config.actions.size()) {
        ImGui::SeparatorText("Action");
        ImGui::TextDisabled(app.config.actions.empty() ? "Click \"Add action\" to start."
                                                       : "Select an action to edit it.");
        return;
    }
    Action& a = app.config.actions[app.selected];
    ImGui::PushID((int) a.id);
    ImGui::SeparatorText(("Action: " + action_label(a)).c_str());
    const float label_w = em * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
    };

    label("Name");
    ImGui::SetNextItemWidth(em * 14);
    if (ImGui::InputText("##name", &a.name)) {
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("To tell actions apart, especially when the sound isn't a word.");
    ImGui::SameLine(0, em);
    if (ImGui::Checkbox("Enabled", &a.enabled)) {
        app.actions_changed();
    }
    ImGui::SetItemTooltip("When off, the action is left out, as if it didn't exist (its recordings are kept).");
    ImGui::SameLine(0, em * 1.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Note");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##note", "optional, e.g. what it does in the game", &a.note)) {
        app.mark_dirty();
    }

    // --- Sound
    label("Sound");
    if (app.recording_action == a.id) {
        push_button_colors(ImVec4(0.6f, 0.15f, 0.15f, 1));
        if (ImGui::Button("Stop recording", ImVec2(em * 8, 0))) {
            app.recognizer.stop_recording();
            collect_recordings();
            app.recording_action = 0;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextColored(COLOR_ERROR, "Make the sound, pause, repeat. Each one becomes a sample.");
        if (app.session_dropped > 0) {
            ImGui::SetCursorPosX(label_w);
            ImGui::TextDisabled("Left out %d short noise%s next to longer sounds.", app.session_dropped,
                app.session_dropped == 1 ? "" : "s");
        }
    } else {
        if (ImGui::Button("Record", ImVec2(em * 8, 0))) {
            app.recording_action = a.id;
            app.session.clear();
            app.session_dropped = 0;
            app.recognizer.start_recording();
        }
        ImGui::SetItemTooltip("Record samples of the sound that triggers this action.\n"
                              "3 to 5 samples work best. Inputs aren't triggered while recording.");
        ImGui::SameLine();
        if (a.samples.empty()) {
            ImGui::TextColored(COLOR_WARN, "No samples yet. Record the sound 3 to 5 times.");
        } else {
            ImGui::TextDisabled("%d sample%s", (int) a.samples.size(), a.samples.size() == 1 ? "" : "s");
        }
    }
    // Samples: play buttons, each with a delete button.
    if (!a.samples.empty()) {
        ImGui::SetCursorPosX(label_w);
        // Samples that sound more like another action's than like the others.
        std::vector<uint64_t> confused_ids;
        std::string confused_with;
        {
            const std::map<uint32_t, SoundInfo> sample_info = app.recognizer.sound_info();
            auto si = sample_info.find(a.id);
            if (si != sample_info.end() && app.sound_mode(a) != SoundMode::Ignore) {
                confused_ids = si->second.confused_sample_ids;
                confused_with = action_label(si->second.similar_action);
            }
        }
        ImGui::PushID("samples");
        int delete_sample_index = -1;
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < a.samples.size(); i++) {
            const Sample& s = a.samples[i];
            char text[32];
            snprintf(text, sizeof(text), "%d: %.2fs", (int) i + 1, s.audio->size() / (float) SAMPLE_RATE);
            const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight() +
                            ImGui::GetStyle().ItemSpacing.x;
            if (i > 0) {
                if (ImGui::GetItemRectMax().x + em + w <= right_edge) {
                    ImGui::SameLine(0, em);
                } else {
                    ImGui::SetCursorPosX(label_w);
                }
            }
            ImGui::PushID((int) i);
            ImGui::BeginGroup();
            const bool confused = std::find(confused_ids.begin(), confused_ids.end(), s.id) != confused_ids.end();
            if (confused) {
                push_button_colors(ImVec4(0.55f, 0.38f, 0.08f, 1));
            }
            if (ImGui::Button(text)) {
                play_audio(*s.audio);
            }
            if (confused) {
                ImGui::PopStyleColor(3);
                ImGui::SetItemTooltip("Play. This one sounds more like \"%s\" than like the other samples.",
                    confused_with.c_str());
            } else {
                ImGui::SetItemTooltip("Play");
            }
            ImGui::SameLine(0, 2);
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
                delete_sample_index = (int) i;
            }
            ImGui::SetItemTooltip("Delete this sample");
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::PopID();
        if (delete_sample_index >= 0) {
            delete_sample(a, (size_t) delete_sample_index);
        }
    }

    label("Kind");
    if (ImGui::Checkbox("Not a command", &a.ignore)) {
        app.actions_changed();
    }
    ImGui::SetItemTooltip("Sounds like this one's samples won't trigger any action. Record what triggers\n"
                          "actions by mistake here (a word you say a lot, a noise).");
    const SoundMode mode = app.sound_mode(a);
    ImGui::SameLine(0, em * 1.5f);
    ImGui::TextDisabled(mode == SoundMode::Ignore         ? "Blocks sounds like these."
                        : mode == SoundMode::Continuous ? "Continuous: matched while it lasts (it uses \"while\")."
                                                          : "Whole sound: matched when it ends.");
    if (mode != SoundMode::Ignore) {
        ImGui::SameLine(0, em * 1.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Strictness");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 8);
        if (ImGui::SliderFloat("##margin", &a.margin, -0.05f, 0.1f, "%.2f")) {
            app.actions_changed();
        }
        ImGui::SetItemTooltip("A sound triggers the action it's closest to, if it's closer to it than to anything else\n"
                              "(other actions, and sounds that aren't commands) by this much. The Match column shows the\n"
                              "difference, the white line is this setting. Raise it if the action triggers on the wrong\n"
                              "sounds, lower it if it doesn't trigger. Or record what it mistakes for it in an action\n"
                              "marked \"Not a command\".");
    }

    const std::map<uint32_t, SoundInfo> info = app.recognizer.sound_info();
    auto si = info.find(a.id);
    if (si != info.end() && si->second.similar_action != 0) {
        const Action* other = app.find(si->second.similar_action);
        if (other != nullptr && mode != SoundMode::Ignore && si->second.confused_samples > 0) {
            ImGui::SetCursorPosX(label_w);
            ImGui::TextColored(COLOR_WARN, "%d of the samples (in orange) sound more like \"%s\" than like the others.",
                si->second.confused_samples, action_label(*other).c_str());
        }
    }
    if (mode == SoundMode::Ignore) {
        ImGui::PopID();
        return;
    }

    // --- What it does
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Does");
    const Parsed& p = app.parse(a);
    ImGui::SameLine(label_w);
    ImGui::BeginGroup();
    if (p.ok || a.notation.find_first_not_of(" \t\r\n") == std::string::npos) {
        Program prog = p.ok ? p.program : Program();
        if (prog.steps.empty()) {
            ImGui::TextDisabled("Nothing yet. Add a step, or start from a preset.");
        }
        ImGui::PushID("steps");
        const bool steps_changed = draw_steps_editor(prog);
        ImGui::PopID();
        if (steps_changed) {
            a.notation = format_program(prog);
            app.actions_changed();
        }
    } else {
        ImGui::TextColored(COLOR_WARN, "The notation below has an error. Fix it there, or");
        ImGui::SameLine();
        if (ImGui::SmallButton("start over")) {
            a.notation.clear();
            app.actions_changed();
        }
    }
    ImGui::EndGroup();

    label("Notation");
    const float button_w = em * 6;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button_w - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputTextWithHint("##notation", "the steps as text, e.g. press A+B 200ms; hold LS=NE", &a.notation)) {
        app.actions_changed();
    }
    ImGui::SetItemTooltip("The steps above, as text. Edit either one.");
    ImGui::SameLine();
    ImGui::Button("Help", ImVec2(button_w, 0));
    if (ImGui::BeginItemTooltip()) {
        notation_help();
        ImGui::EndTooltip();
    }
    if (!p.ok && !a.notation.empty()) {
        ImGui::SetCursorPosX(label_w);
        ImGui::TextColored(COLOR_WARN, "%s", p.error.c_str());
    }

    // Test: like making the sound. Continuous actions run while the button is held.
    ImGui::SetCursorPosX(label_w);
    ImGui::BeginDisabled(!p.ok || !app.running || !a.enabled);
    const bool continuous = app.sound_mode(a) == SoundMode::Continuous;
    ImGui::Button("Test action", ImVec2(em * 8, 0));
    if (ImGui::IsItemActivated()) {
        app.engine.post({ a.id, continuous ? TriggerType::Start : TriggerType::Fire });
    }
    if (ImGui::IsItemDeactivated() && continuous) {
        app.engine.post({ a.id, TriggerType::Stop });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(continuous ? "Runs the action as if you made the sound, for as long as you hold the button."
                                     : "Runs the action as if you made the sound.");
    if (!app.running) {
        ImGui::SameLine();
        ImGui::TextDisabled("(start sending to test)");
    }
    ImGui::PopID();
}

static void draw_sound_settings(const RecognizerStatus& st) {
    App& app = g_app;
    Config& cfg = app.config;
    const float em = ImGui::GetFontSize();
    draw_level_meter(st, ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled(st.recording ? "Recording" : st.in_sound ? "Hearing a sound" : "Quiet");
    ImGui::SameLine(em * 9);
    ImGui::TextDisabled("background %.0f dB", st.floor_db);
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::SliderFloat("Gate", &cfg.vad_margin_db, 3.0f, 30.0f, "%.0f dB")) {
        app.recognizer.set_vad(cfg.vad_margin_db, cfg.end_delay_ms);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("How much louder than the background a sound must be to count.\n"
                          "Raise it if background noise triggers things.");
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::SliderInt("End delay", &cfg.end_delay_ms, 50, 500, "%d ms")) {
        app.recognizer.set_vad(cfg.vad_margin_db, cfg.end_delay_ms);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("Quiet time that ends a sound. Whole sounds are matched when they end, so shorter is\n"
                          "faster, but words with pauses in them (\"stop\") need enough to stay in one piece.");
}

static void draw_activity_log() {
    App& app = g_app;
    std::deque<LogEntry> log;
    {
        std::lock_guard<std::mutex> lock(app.log_mutex);
        log = app.log;
    }
    if (ImGui::BeginChild("log", ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 5.5f),
            ImGuiChildFlags_Borders)) {
        if (log.empty()) {
            ImGui::TextDisabled("Recognized sounds will show up here.");
        }
        const ULONGLONG now = GetTickCount64();
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            const float age = (now - it->time) / 1000.0f;
            const char* what = it->type == TriggerType::Fire ? "heard" : it->type == TriggerType::Start ? "started" : "stopped";
            ImGui::TextDisabled("%6.1fs ago", age);
            ImGui::SameLine();
            if (it->type == TriggerType::Stop) {
                ImGui::Text("%s %s", action_label(it->action_id).c_str(), what);
            } else {
                ImGui::Text("%s %s (%.2f)", action_label(it->action_id).c_str(), what, it->score);
            }
            if (!it->sent && it->type != TriggerType::Stop) {
                ImGui::SameLine();
                ImGui::TextDisabled("- not sent");
            }
        }
    }
    ImGui::EndChild();
}

static void draw_ui() {
    App& app = g_app;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_top_bar();
    ImGui::Spacing();

    const RecognizerStatus st = app.recognizer.status();
    const float right_w = std::max(em * 20, ImGui::GetContentRegionAvail().x * 0.34f);
    const float left_w = ImGui::GetContentRegionAvail().x - right_w - ImGui::GetStyle().ItemSpacing.x;

    if (ImGui::BeginChild("left", ImVec2(left_w, 0))) {
        ImGui::SeparatorText("Actions");
        draw_actions_table(st);
        ImGui::Spacing();
        draw_action_details();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("right", ImVec2(0, 0))) {
        ImGui::SeparatorText("Output");
        draw_output();
        ImGui::SeparatorText("Sound detection");
        draw_sound_settings(st);
        ImGui::SeparatorText("Activity");
        draw_activity_log();
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            capture_window_placement();
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_MOVE:
            capture_window_placement();
            return 0;
        case WM_CLOSE:
            capture_window_placement();
            break;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (900 * s);
            mmi->ptMinTrackSize.y = (LONG) (600 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    App& app = g_app;
    app.config_path = app_data_dir() + L"\\settings.json";
    load_config(app.config_path, app.config);
    for (const Action& a : app.config.actions) {
        app.next_action_id = std::max(app.next_action_id, a.id + 1);
    }
    if (wcsstr(GetCommandLineW(), L"--start") != nullptr) {
        app.running = true;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VoiceHidTransmitter";
    RegisterClassExW(&wc);

    // Restore the window where it was last time, but only if its title bar
    // would still be reachable (monitors may have changed since).
    const WindowRect& saved = app.config.window;
    bool restore = false;
    if (saved.valid) {
        RECT title = { saved.x, saved.y, saved.x + saved.w, saved.y + GetSystemMetrics(SM_CYCAPTION) };
        if (HMONITOR mon = MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi = { sizeof(mi) };
            RECT visible;
            restore = GetMonitorInfoW(mon, &mi) && IntersectRect(&visible, &title, &mi.rcWork) &&
                      visible.right - visible.left >= 100;
        }
    }

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = restore
        ? CreateWindowW(wc.lpszClassName, L"Voice HID Transmitter", WS_OVERLAPPEDWINDOW,
              saved.x, saved.y, saved.w, saved.h, nullptr, nullptr, instance, nullptr)
        : CreateWindowW(wc.lpszClassName, L"Voice HID Transmitter", WS_OVERLAPPEDWINDOW,
              (int) (80 * primary_scale), (int) (80 * primary_scale), (int) (1200 * primary_scale),
              (int) (820 * primary_scale), nullptr, nullptr, instance, nullptr);
    app.main_hwnd = hwnd;
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Voice HID Transmitter", MB_ICONERROR);
        return 1;
    }

    // Detections arrive on the audio thread.
    app.recognizer.set_callback([](const Detection& d) {
        App& app = g_app;
        const bool sent = app.sending.load();
        if (sent) {
            app.engine.post({ d.action_id, d.type });
        }
        LogEntry e;
        e.time = GetTickCount64();
        e.action_id = d.action_id;
        e.type = d.type;
        e.score = d.score;
        e.sent = sent;
        std::lock_guard<std::mutex> lock(app.log_mutex);
        app.log.push_back(e);
        while (app.log.size() > 100) {
            app.log.pop_front();
        }
        if (d.type != TriggerType::Stop) {
            app.last_triggered[d.action_id] = e.time;
        }
    });
    app.recognizer.set_vad(app.config.vad_margin_db, app.config.end_delay_ms);
    {
        const std::wstring cache = app_data_dir() + L"\\cache";
        CreateDirectoryW(cache.c_str(), nullptr);
        app.recognizer.set_cache_dir(cache);
    }
    app.actions_changed();
    app.config_dirty = false;
    app.engine.set_receiver(app.config.receiver_ip);
    app.engine.start();
    app.devices = list_capture_devices();
    app.audio.set_device(app.config.mic_id);
    app.audio.start([](const float* samples, size_t n) { g_app.recognizer.process(samples, n); },
        []() { g_app.recognizer.reset_stream(); });

    if (restore) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = { saved.x, saved.y, saved.x + saved.w, saved.y + saved.h };
        wp.showCmd = show_cmd;
        if (saved.maximized && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW || show_cmd == SW_SHOWDEFAULT)) {
            wp.showCmd = SW_SHOWMAXIMIZED;
        }
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    g_track_placement = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        app.engine.set_active(app.running);
        app.sending = app.running;
        collect_recordings();
        if (app.config_dirty && GetTickCount64() - app.config_dirty_since >= 1000) {
            save_now();
        }

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    app.running = false;
    app.sending = false;
    app.audio.stop();
    app.recognizer.stop();
    app.engine.set_active(false);
    app.engine.stop();
    PlaySoundW(nullptr, nullptr, 0);
    if (app.config_dirty) {
        save_now();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();
    CoUninitialize();
    return 0;
}
