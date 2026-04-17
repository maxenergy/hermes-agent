#include "hermes/config/default_config.hpp"

namespace hermes::config {

namespace {

nlohmann::json make_default_config() {
    using nlohmann::json;
    json cfg = json::object();

    // --- LLM / provider core ---
    // Default: ChatGPT Codex OAuth — piggy-backs on the access_token
    // maintained by the Codex CLI at ``~/.codex/auth.json``.  Override
    // via `hermes login <provider>` or by setting ``provider`` / ``model``
    // explicitly in ``~/.hermes/config.yaml``.  reasoning_effort follows
    // Codex's string convention ("none"|"low"|"medium"|"high"); the
    // agent layer translates it to the integer form the LLM API wants.
    cfg["model"] = "gpt-5.4";
    cfg["provider"] = "openai-codex";
    cfg["base_url"] = "";
    cfg["reasoning_effort"] = "medium";

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

    // --- Security (v5 -> v6) ---
    cfg["security"] = {
        {"redact_secrets", true},
        {"tirith_enabled", true},
        {"tirith_path", "tirith"},
        {"tirith_timeout", 5},
        {"tirith_fail_open", true},
    };

    // --- Logging (v5 -> v6) ---
    cfg["logging"] = {
        {"level", "INFO"},
        {"max_size_mb", 5},
        {"backup_count", 3},
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
