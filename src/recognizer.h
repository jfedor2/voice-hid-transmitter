// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "features.h"

// How an action's sound is detected.
enum class SoundMode {
    Whole,       // the whole sound is matched when it ends (words, clicks)
    Continuous,  // matched while it lasts: starts ~150 ms in, stops when it stops (whistles, hums)
    Ignore,      // not a command: sounds like these are kept from triggering anything
};

using Audio = std::vector<float>;  // 16 kHz mono

struct SampleData {
    uint64_t id = 0;  // unique, used to cache features
    std::shared_ptr<const Audio> audio;
};

struct SoundSpec {
    uint32_t action_id = 0;
    SoundMode mode = SoundMode::Whole;
    // What it takes to trigger: how much closer the sound must be to this
    // action's samples than to anything else (0 = just closer).
    float threshold = 0.0f;
    std::vector<SampleData> samples;
};

// What the model builder found out about a sound, for the UI.
struct SoundInfo {
    int samples = 0;
    // Another action this sound is easily mistaken for, or 0, and which of its
    // samples are closer to that action's samples than to its own other samples.
    uint32_t similar_action = 0;
    int confused_samples = 0;
    std::vector<uint64_t> confused_sample_ids;
};

// A sound heard while recording.
struct RecordedClip {
    Audio audio;  // with some quiet before and after
    // The loud part, in ms of stream time (since the microphone was opened).
    uint64_t loud_start_ms = 0;
    uint64_t loud_end_ms = 0;

    uint64_t loud_ms() const { return loud_end_ms - loud_start_ms; }
};

struct Detection {
    uint32_t action_id = 0;
    TriggerType type = TriggerType::Fire;
    float score = 0.0f;
};

struct RecognizerStatus {
    float level_db = -100.0f;
    float floor_db = -100.0f;
    float threshold_db = -100.0f;
    bool in_sound = false;
    bool recording = false;
    // Match score per action (how much closer to it than to anything else):
    // for continuous sounds the current one, for whole sounds the one from the
    // last sound heard.
    std::map<uint32_t, float> scores;
    uint64_t sounds_heard = 0;
    uint32_t active_continuous = 0;  // action whose continuous sound is on, or 0
    // Since the microphone was (re)opened: whether anything above digital
    // silence came in, and for how long it has been open.
    bool signal_seen = false;
    float stream_seconds = 0.0f;
};

// Turns audio into detections. process() runs on the audio thread; the
// templates are (re)built on a background thread whenever the sounds change.
//
// Sounds are found with an energy gate that tracks the background noise
// level.
//
// A whole sound is turned into vectors (see sound_vectors.h) and compared to
// the vectors of every action's samples, and of things that aren't commands:
// the built-in background (common words, noises), parts of the recorded
// sounds (the "ft" of "left" alone shouldn't count as "left"), and sounds the
// user marked as "not a command". It triggers the action it's closest to, if it's
// closer to it than to anything else by the action's threshold. So actions are
// told apart by what makes them different, and a sound that isn't close to
// any action in particular triggers nothing.
//
// Continuous sounds are matched the same way, every 50 ms while a sound
// lasts, from what has been heard of it so far (the first 50, 100, ... ms,
// then the last 500 ms), against the same chunks of the recorded samples and
// of things that aren't continuous sounds (the other actions' sounds, the
// background). A continuous action starts when it wins 3 times in a row, and
// stops when the sound stops or something else wins for 200 ms.
class Recognizer {
public:
    Recognizer();
    ~Recognizer();

    // Stops building (abandoning a build in progress). Call before the
    // program exits: the build uses data that's destroyed at exit.
    void stop();

    // Called on the audio thread. Set before audio starts.
    void set_callback(std::function<void(const Detection&)> callback) { callback_ = std::move(callback); }

    void set_sounds(const std::vector<SoundSpec>& sounds);
    // Where to keep the samples' features between runs (computing them takes
    // a while). Empty = don't keep them. Set before set_sounds().
    void set_cache_dir(const std::wstring& dir) { cache_dir_ = dir; }
    void set_vad(float margin_db, int end_delay_ms);
    std::map<uint32_t, SoundInfo> sound_info();
    bool building();

    // While recording, every sound heard becomes a sample instead of being matched.
    void start_recording();
    void stop_recording();
    std::vector<RecordedClip> take_recorded();

    // Audio thread.
    void process(const float* samples, size_t n);
    void reset_stream();

    RecognizerStatus status();

    // Features of a sample (public for the helpers that save them).
    struct Cached;

private:
    struct Model;

    void builder_main();
    void build(const std::vector<SoundSpec>& specs);
    void on_frame(const Frame& f, const Model& model);
    void end_segment(const Model& model);
    void match_whole(const Audio& audio, const Model& model);
    void continuous_frame(bool loud, const Model& model);
    void stop_continuous();
    void emit(uint32_t action_id, TriggerType type, float score);

    std::function<void(const Detection&)> callback_;

    // Model builder.
    std::thread builder_;
    std::mutex build_mutex_;
    std::condition_variable build_cv_;
    std::vector<SoundSpec> pending_specs_;
    bool specs_pending_ = false;
    bool quit_ = false;
    std::atomic<bool> cancel_{ false };  // stop building, we're quitting
    std::atomic<bool> building_{ false };
    std::map<uint64_t, std::shared_ptr<Cached>> cache_;  // builder thread only
    std::wstring cache_dir_;

    // Shared.
    std::mutex mutex_;
    std::shared_ptr<const Model> model_;
    std::map<uint32_t, SoundInfo> info_;
    RecognizerStatus status_;
    std::vector<RecordedClip> recorded_;
    std::atomic<bool> recording_{ false };
    std::atomic<float> margin_db_{ 12.0f };
    std::atomic<int> end_delay_ms_{ 150 };

    // Audio thread only.
    FeatureFrontend frontend_;
    std::vector<Frame> new_frames_;
    uint64_t frame_index_ = 0;
    bool signal_seen_ = false;
    float floor_db_ = 0.0f;
    bool floor_init_ = false;
    std::vector<float> floor_history_;
    size_t floor_pos_ = 0;
    std::vector<float> floor_sorted_;
    std::vector<Frame> preroll_;
    bool in_segment_ = false;
    uint64_t seg_start_ = 0;  // frame index
    uint64_t first_loud_ = 0;
    uint64_t last_loud_ = 0;
    std::vector<Frame> seg_frames_;
    bool seg_continuous_ = false;  // a continuous sound triggered during this segment
    bool was_recording_ = false;
    // Recent audio, for recording and whole-sound matching: samples from
    // absolute index audio_start_.
    std::vector<float> audio_;
    uint64_t audio_start_ = 0;
    uint64_t samples_seen_ = 0;
    // Continuous matching.
    std::shared_ptr<const Model> stream_model_;  // the model the state below is for
    uint32_t cont_active_ = 0;     // the action whose sound is on, or 0
    uint32_t cont_candidate_ = 0;  // the action winning the last decisions
    int cont_count_ = 0;           // decisions it has won in a row
    int cont_lost_ = 0;            // decisions the active one has lost in a row
    int cont_quiet_ = 0;           // quiet frames in a row
    std::map<uint32_t, float> scores_;
};
