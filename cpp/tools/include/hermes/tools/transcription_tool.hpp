// Phase 8: Audio transcription tool — whisper / cloud STT providers.
//
// Registers the `transcribe_audio` tool, backed by whisper.cpp CLI
// (`whisper-cli`, `whisper-cpp`, `main`) or OpenAI's Python `whisper` /
// `faster-whisper` CLIs — whichever is first found on $PATH.
//
// For tests a pluggable backend hook is exposed via
// `set_transcription_backend_for_testing` so unit tests don't require a
// whisper binary or a real model file.
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace hermes::tools {

// Inputs passed to a transcription backend.
struct TranscriptionRequest {
    std::string audio_path;
    std::string language;   // "auto" if unset
    std::string model;      // model name / size (e.g. "base", "small")
};

// Result from a transcription backend.  `ok == false` means the backend
// either wasn't available or failed; `error` carries a human-readable
// reason.  When `ok == true` at minimum `text` should be populated, and
// optionally `segments` / `language` for richer output.
struct TranscriptionResult {
    bool ok = false;
    std::string text;
    std::string language;
    std::string backend;        // e.g. "whisper-cli", "faster-whisper"
    nlohmann::json segments;    // array (possibly empty)
    std::string error;          // human-readable error if !ok
};

using TranscriptionBackend =
    std::function<TranscriptionResult(const TranscriptionRequest&)>;

// Install a custom backend (tests).  Passing an empty std::function
// restores the default (subprocess) backend.  Thread-safe.
void set_transcription_backend_for_testing(TranscriptionBackend backend);

void register_transcription_tools();

}  // namespace hermes::tools
