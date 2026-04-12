#include "hermes/tools/voice_mode_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <string>

namespace hermes::tools {

// ---------------------------------------------------------------------------
// VoiceSession
// ---------------------------------------------------------------------------

VoiceSession& VoiceSession::instance() {
    static VoiceSession s;
    return s;
}

VoiceState VoiceSession::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

void VoiceSession::start(const nlohmann::json& config) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = VoiceState::Listening;
    config_ = config;
}

void VoiceSession::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = VoiceState::Inactive;
}

nlohmann::json VoiceSession::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j;
    j["state"] = state_string(state_);
    j["config"] = config_;
    return j;
}

void VoiceSession::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = VoiceState::Inactive;
    config_ = nlohmann::json::object();
}

std::string VoiceSession::state_string(VoiceState s) {
    switch (s) {
        case VoiceState::Inactive:   return "inactive";
        case VoiceState::Listening:  return "listening";
        case VoiceState::Processing: return "processing";
        case VoiceState::Speaking:   return "speaking";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Tool handler
// ---------------------------------------------------------------------------

namespace {

std::string handle_voice_mode(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    const auto action = args.at("action").get<std::string>();
    auto& session = VoiceSession::instance();

    if (action == "start") {
        nlohmann::json config;
        if (args.contains("config")) {
            config = args["config"];
        }
        session.start(config);
        return tool_result({{"started", true}, {"state", "listening"}});
    }

    if (action == "stop") {
        session.stop();
        return tool_result({{"stopped", true}});
    }

    if (action == "status") {
        return tool_result(session.status());
    }

    return tool_error("unknown voice_mode action '" + action +
                      "' — expected start|stop|status");
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_voice_tools() {
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "voice_mode";
    e.toolset = "voice";
    e.description = "Manage voice input/output session";
    e.emoji = "\xF0\x9F\x8E\xA4";  // microphone2
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"start", "stop", "status"})},
            {"description", "Voice mode action"}}},
          {"config",
           {{"type", "object"},
            {"description",
             "Optional config: {stt_model, tts_voice, tts_provider}"}}}}},
        {"required", nlohmann::json::array({"action"})}};
    e.handler = handle_voice_mode;
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
