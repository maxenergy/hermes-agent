#include "hermes/tools/transcription_tool.hpp"
#include "hermes/tools/registry.hpp"

#include "hermes/environments/local.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

const std::vector<std::string> kAudioExtensions = {
    ".mp3", ".wav", ".ogg", ".flac", ".m4a", ".webm"};

bool is_audio_extension(const std::string& ext) {
    auto lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return std::find(kAudioExtensions.begin(), kAudioExtensions.end(),
                     lower) != kAudioExtensions.end();
}

std::string handle_transcribe(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    const auto audio_path = args.at("audio_path").get<std::string>();
    const auto model =
        args.contains("model") ? args["model"].get<std::string>()
                               : std::string("whisper");

    // Check file exists.
    if (!std::filesystem::exists(audio_path)) {
        return tool_error("audio file not found");
    }

    // Check extension is a supported audio format.
    auto ext = std::filesystem::path(audio_path).extension().string();
    if (!is_audio_extension(ext)) {
        return tool_error(
            "unsupported audio format '" + ext +
            "' — supported: .mp3, .wav, .ogg, .flac, .m4a, .webm");
    }

    const auto language =
        args.contains("language") ? args["language"].get<std::string>()
                                  : std::string("auto");

    // Try whisper.cpp CLI first, then faster-whisper.
    hermes::environments::LocalEnvironment env;
    hermes::environments::ExecuteOptions opts;
    opts.timeout = std::chrono::seconds(300);

    // Attempt whisper (whisper.cpp)
    std::string cmd = "whisper --model base --language " + language +
                      " --output-format json " + audio_path;
    auto result = env.execute("which whisper >/dev/null 2>&1 && " + cmd, opts);

    if (result.exit_code != 0) {
        // Fall back to faster-whisper
        cmd = "faster-whisper --model base --language " + language +
              " --output_format json " + audio_path;
        result = env.execute("which faster-whisper >/dev/null 2>&1 && " + cmd, opts);
    }

    if (result.exit_code != 0) {
        return tool_error(
            "no whisper binary found in PATH — install whisper.cpp or faster-whisper");
    }

    // Parse JSON output from whisper.
    auto parsed = nlohmann::json::parse(result.stdout_text, nullptr, false);
    if (parsed.is_discarded()) {
        // Return raw text if not valid JSON.
        nlohmann::json r;
        r["text"] = result.stdout_text;
        r["model"] = model;
        return tool_result(r);
    }

    nlohmann::json r;
    r["transcription"] = parsed;
    r["model"] = model;
    return tool_result(r);
}

}  // namespace

void register_transcription_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "transcribe_audio";
    e.toolset = "voice";
    e.description = "Transcribe an audio file to text";
    e.emoji = "\xF0\x9F\x8E\x99";  // microphone
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"audio_path",
           {{"type", "string"},
            {"description", "Path to the audio file to transcribe"}}},
          {"model",
           {{"type", "string"},
            {"description", "Transcription model (default whisper)"}}},
          {"language",
           {{"type", "string"},
            {"description", "Language code (optional)"}}}}},
        {"required", nlohmann::json::array({"audio_path"})}};
    e.handler = handle_transcribe;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
