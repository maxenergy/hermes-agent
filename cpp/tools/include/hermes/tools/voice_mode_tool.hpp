// Phase 8: Voice mode tool — session state machine + push-to-talk
// capture driver.
//
// The session is a 4-state machine:
//
//     Inactive ── start ─▶ Listening ── record ─▶ Listening (recording)
//         ▲                    │                       │
//         │                    │                       ├─ transcribe ─▶ Processing ─▶ Listening
//         └── stop ────────────┘                       └─ cancel ─────▶ Listening
//
// Audio capture is delegated to a pluggable recorder — the default
// recorder shells out to `arecord` (ALSA), `parecord` (PulseAudio), or
// `rec` (SoX) depending on which is on $PATH.  Tests install a mock
// recorder via `set_recorder_for_testing` so unit tests don't require a
// working microphone or audio daemon.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hermes::tools {

enum class VoiceState { Inactive, Listening, Processing, Speaking };

// Abstract audio recorder — one instance per `voice_mode` session.
// The default implementation uses arecord/parecord/sox; tests inject a
// fake via `set_recorder_for_testing`.
class AudioRecorder {
public:
    virtual ~AudioRecorder() = default;

    // Begin capture to a fresh file.  Returns the output path on
    // success or an empty string if capture could not start (in which
    // case `error` is populated).  Implementations should non-block.
    virtual std::string start(std::string& error) = 0;

    // Stop the in-progress capture.  Returns the final wav path (same
    // as the value returned by start), or empty string if no capture
    // was active / the file is empty.
    virtual std::string stop() = 0;

    // Abort without producing a usable wav.
    virtual void cancel() = 0;

    virtual bool is_recording() const = 0;

    // Elapsed recording time (0 when idle).
    virtual std::chrono::milliseconds elapsed() const = 0;

    // Human-readable name for telemetry (e.g. "arecord", "mock").
    virtual std::string backend_name() const = 0;
};

using RecorderFactory = std::function<std::unique_ptr<AudioRecorder>()>;

// Install a custom recorder factory for tests.  Pass an empty
// std::function to restore the default.
void set_recorder_factory_for_testing(RecorderFactory factory);

// Events emitted during a capture session.  The Python reference
// implementation also emits `partial` events as audio streams in; our
// CLI backends don't support streaming, so we only ever emit one
// `final` event per transcription.
struct VoiceEvent {
    std::string kind;          // "partial" | "final" | "error"
    std::string text;
    std::string language;
    std::chrono::steady_clock::time_point ts;
};

// Thread-safe voice session state machine.  The session owns a single
// AudioRecorder for its lifetime (recreated on reset()).
class VoiceSession {
public:
    static VoiceSession& instance();

    VoiceState state() const;

    // Install config (stt_model, tts_voice, ...) and transition to
    // Listening.  Idempotent.
    void start(const nlohmann::json& config);

    // Cancel any in-flight recording and transition to Inactive.
    void stop();

    nlohmann::json status() const;

    // Begin capturing microphone audio.  Must be called after start().
    // Returns the recording path (or empty string + error via status
    // on failure).
    std::string begin_recording(std::string& error);

    // Stop recording (if active) and return the final wav path.  Does
    // not transcribe — caller chains transcribe_audio itself.
    std::string finish_recording();

    // Full round trip: stop recording + dispatch to transcription +
    // emit events.  Returns the final transcript text.  Populates
    // `err` on failure.
    std::string transcribe_last(const std::string& language,
                                 const std::string& model,
                                 std::string& err);

    // Events captured so far (test hook + UI feedback).
    std::vector<VoiceEvent> drain_events();

    // Testing helper.
    void reset();

private:
    VoiceSession() = default;
    VoiceSession(const VoiceSession&) = delete;
    VoiceSession& operator=(const VoiceSession&) = delete;

    static std::string state_string(VoiceState s);

    void ensure_recorder_locked_();

    mutable std::mutex mu_;
    VoiceState state_ = VoiceState::Inactive;
    nlohmann::json config_;
    std::unique_ptr<AudioRecorder> recorder_;
    std::string last_recording_path_;
    std::vector<VoiceEvent> events_;
};

void register_voice_tools();

}  // namespace hermes::tools
