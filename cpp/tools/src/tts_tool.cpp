#include "hermes/tools/tts_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

        nlohmann::json result;
        result["audio_path"] = output_path.string();
        result["format"] = "mp3";
        result["command"] = cmd.str();
        return tool_result(result);
    }

    if (provider == "openai") {
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key || api_key[0] == '\0') {
            return tool_error("OPENAI_API_KEY not set");
        }

        auto* transport = hermes::llm::get_default_transport();
        // CurlTransport is always available when built with libcurl.
        assert(transport && "HTTP transport should always be available");

        // Map voice name — default to "alloy" if the edge-tts voice was used.
        std::string openai_voice = voice;
        if (voice.find("Neural") != std::string::npos) {
            openai_voice = "alloy";
        }

        nlohmann::json req_body;
        req_body["model"] = "tts-1";
        req_body["input"] = text;
        req_body["voice"] = openai_voice;

        std::unordered_map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json";
        headers["Authorization"] = std::string("Bearer ") + api_key;

        auto resp = transport->post_json(
            "https://api.openai.com/v1/audio/speech", headers,
            req_body.dump());

        if (resp.status_code != 200) {
            return tool_error("OpenAI TTS API error",
                              {{"status", resp.status_code},
                               {"body", resp.body}});
        }

        // Write audio bytes to temp file.
        auto tmpdir = std::filesystem::temp_directory_path();
        auto output_path = tmpdir / ("hermes_tts_" +
                                     (ctx.task_id.empty() ? "out" : ctx.task_id) +
                                     ".mp3");
        {
            std::ofstream ofs(output_path, std::ios::binary);
            ofs.write(resp.body.data(),
                      static_cast<std::streamsize>(resp.body.size()));
        }

        nlohmann::json result;
        result["audio_path"] = output_path.string();
        result["format"] = "mp3";
        result["provider"] = "openai";
        return tool_result(result);
    }

    if (provider == "elevenlabs") {
        const char* api_key = std::getenv("ELEVENLABS_API_KEY");
        if (!api_key || api_key[0] == '\0') {
            return tool_error("ELEVENLABS_API_KEY not set");
        }

        auto* transport = hermes::llm::get_default_transport();
        // CurlTransport is always available when built with libcurl.
        assert(transport && "HTTP transport should always be available");

        // Use the voice parameter as the voice_id.
        std::string voice_id = voice;
        if (voice.find("Neural") != std::string::npos) {
            voice_id = "21m00Tcm4TlvDq8ikWAM";  // default Rachel
        }

        nlohmann::json req_body;
        req_body["text"] = text;

        std::unordered_map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json";
        headers["xi-api-key"] = api_key;

        auto resp = transport->post_json(
            "https://api.elevenlabs.io/v1/text-to-speech/" + voice_id,
            headers, req_body.dump());

        if (resp.status_code != 200) {
            return tool_error("ElevenLabs TTS API error",
                              {{"status", resp.status_code},
                               {"body", resp.body}});
        }

        // Write audio bytes to temp file.
        auto tmpdir = std::filesystem::temp_directory_path();
        auto output_path = tmpdir / ("hermes_tts_" +
                                     (ctx.task_id.empty() ? "out" : ctx.task_id) +
                                     ".mp3");
        {
            std::ofstream ofs(output_path, std::ios::binary);
            ofs.write(resp.body.data(),
                      static_cast<std::streamsize>(resp.body.size()));
        }

        nlohmann::json result;
        result["audio_path"] = output_path.string();
        result["format"] = "mp3";
        result["provider"] = "elevenlabs";
        return tool_result(result);
    }

    if (provider == "neutts") {
        // NeuTTS is a local neural TTS CLI.  Shell out when the binary is
        // installed; otherwise tell the caller how to get it.
        auto resolve = [](const std::string& bin) {
            if (bin.find('/') != std::string::npos) {
                return ::access(bin.c_str(), X_OK) == 0;
            }
            const char* path_env = ::getenv("PATH");
            if (!path_env) return false;
            std::string p;
            for (const char* c = path_env; *c; ++c) {
                if (*c == ':') {
                    if (!p.empty() && ::access((p + "/" + bin).c_str(), X_OK) == 0) return true;
                    p.clear();
                } else {
                    p.push_back(*c);
                }
            }
            return !p.empty() && ::access((p + "/" + bin).c_str(), X_OK) == 0;
        };
        if (!resolve("neutts")) {
            return tool_error(
                "neutts binary not found on PATH — "
                "install with 'pip install neutts' or 'uv tool install neutts'");
        }
        auto tmpdir = std::filesystem::temp_directory_path();
        auto output_path =
            tmpdir / ("hermes_tts_" +
                      (ctx.task_id.empty() ? std::string("out") : ctx.task_id) +
                      ".wav");
        std::ostringstream cmd;
        cmd << "neutts --text " << shell_escape(text)
            << " --voice " << shell_escape(voice)
            << " --output " << shell_escape(output_path.string())
            << " 2>&1";
        int rc = std::system(cmd.str().c_str());
        if (rc != 0) {
            return tool_error(
                "neutts failed",
                {{"exit_code", rc}, {"command", cmd.str()}});
        }
        nlohmann::json result;
        result["audio_path"] = output_path.string();
        result["format"] = "wav";
        result["provider"] = "neutts";
        return tool_result(result);
    }

    return tool_error("Unknown TTS provider: " + provider);
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
             nlohmann::json::array({"edge", "elevenlabs", "openai", "neutts"})},
            {"description",
             "TTS provider (default edge)"}}}}},
        {"required", nlohmann::json::array({"text"})}};
    e.handler = handle_tts;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
