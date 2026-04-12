// Phase 8: Voice mode tool — session state machine for voice I/O.
#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>

namespace hermes::tools {

enum class VoiceState { Inactive, Listening, Processing, Speaking };

// Thread-safe voice session state machine.  Actual audio capture/playback
// is Phase 14; this manages the logical state + configuration only.
class VoiceSession {
public:
    static VoiceSession& instance();

    VoiceState state() const;
    void start(const nlohmann::json& config);  // -> Listening
    void stop();                                // -> Inactive
    nlohmann::json status() const;              // current state + config

    // Testing helper.
    void reset();

private:
    VoiceSession() = default;
    VoiceSession(const VoiceSession&) = delete;
    VoiceSession& operator=(const VoiceSession&) = delete;

    static std::string state_string(VoiceState s);

    mutable std::mutex mu_;
    VoiceState state_ = VoiceState::Inactive;
    nlohmann::json config_;
};

void register_voice_tools();

}  // namespace hermes::tools
