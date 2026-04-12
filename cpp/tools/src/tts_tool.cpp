#include "hermes/tools/tts_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace hermes::tools {

namespace {

// Shell-escape a string for safe embedding in a shell command.
std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string handle_tts(const nlohmann::json& args,
                       const ToolContext& ctx) {
    const auto text = args.at("text").get<std::string>();
    const auto voice =
        args.contains("voice") ? args["voice"].get<std::string>()
                               : std::string("en-US-AriaNeural");
    const auto provider =
        args.contains("provider") ? args["provider"].get<std::string>()
                                  : std::string("edge");

    if (provider == "edge") {
        // Construct the edge-tts command.
        auto tmpdir = std::filesystem::temp_directory_path();
        auto output_path = tmpdir / ("hermes_tts_" +
                                     (ctx.task_id.empty() ? "out" : ctx.task_id) +
                                     ".mp3");

        std::ostringstream cmd;
        cmd << "edge-tts"
            << " --text " << shell_escape(text)
            << " --voice " << shell_escape(voice)
            << " -w " << shell_escape(output_path.string());

        // We do NOT actually execute here — just return the command
        // and path.  Real execution requires LocalEnvironment wiring
        // (Phase 12).
        nlohmann::json result;
        result["audio_path"] = output_path.string();
        result["format"] = "mp3";
        result["command"] = cmd.str();
        return tool_result(result);
    }

    // elevenlabs / openai providers need HttpTransport.
    return tool_error("HTTP-based TTS provider '" + provider +
                      "' not available — rebuild with cpr");
}

}  // namespace

void register_tts_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "text_to_speech";
    e.toolset = "tts";
    e.description = "Convert text to speech audio";
    e.emoji = "\xF0\x9F\x94\x8A";  // speaker
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"text",
           {{"type", "string"}, {"description", "Text to synthesize"}}},
          {"voice",
           {{"type", "string"},
            {"description", "Voice name (optional)"}}},
          {"provider",
           {{"type", "string"},
            {"enum",
             nlohmann::json::array({"edge", "elevenlabs", "openai"})},
            {"description",
             "TTS provider (default edge)"}}}}},
        {"required", nlohmann::json::array({"text"})}};
    e.handler = handle_tts;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
