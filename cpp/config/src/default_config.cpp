#include "hermes/config/default_config.hpp"

namespace hermes::config {

namespace {

nlohmann::json make_default_config() {
    using nlohmann::json;
    json cfg = json::object();

    // --- LLM / provider core ---
    cfg["model"] = "";
    cfg["provider"] = "";
    cfg["base_url"] = "";

    // --- Terminal sandbox backend ---
    cfg["terminal"] = {
        {"backend", "local"},
        {"timeout", 180},
        {"use_pty", true},
        {"docker_image", "nikolaik/python-nodejs:python3.11-nodejs20"},
    };

    // --- Toolsets ---
    cfg["tools"] = {
        {"enabled_toolsets", json::array({"hermes-cli"})},
        {"disabled_toolsets", json::array()},
    };

    // --- Display / UX ---
    cfg["display"] = {
        {"skin", "default"},
        {"tool_progress_command", false},
        {"background_process_notifications", true},
    };

    // --- Memory (scaffold; real keys land in Phase 2+) ---
    cfg["memory"] = {
        {"memory_enabled", true},
        {"user_profile_enabled", true},
        {"memory_char_limit", 2200},
        {"user_char_limit", 1375},
        {"provider", ""},
    };

    // --- Messaging gateway (scaffold) ---
    cfg["messaging"] = json::object();

    // --- Web / browser tooling (scaffold) ---
    cfg["web"] = json::object();

    // --- TTS (scaffold) ---
    cfg["tts"] = {
        {"provider", "edge"},
    };

    cfg["_config_version"] = kCurrentConfigVersion;
    return cfg;
}

}  // namespace

const nlohmann::json& default_config() {
    static const nlohmann::json kDefault = make_default_config();
    return kDefault;
}

}  // namespace hermes::config
