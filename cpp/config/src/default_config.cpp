#include "hermes/config/default_config.hpp"

namespace hermes::config {

namespace {

nlohmann::json make_default_config() {
    using nlohmann::json;
    json cfg = json::object();

    // --- LLM / provider core ---
    // Default: ChatGPT Codex OAuth.  Hermes owns its own Codex auth
    // state in ``~/.hermes/auth.json`` / ``~/.hermes/.env`` — run
    // ``hermes auth openai-codex`` to log in (the setup flow will
    // offer a one-time import from ``~/.codex/auth.json`` if the
    // Codex CLI has already been used).  Override via
    // ``hermes login <provider>`` or by setting ``provider`` /
    // ``model`` explicitly in ``~/.hermes/config.yaml``.
    // reasoning_effort follows Codex's string convention
    // ("none"|"low"|"medium"|"high"); the agent layer translates it
    // to the integer form the LLM API wants.
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

    // --- Browser (v15): optional persistent CDP endpoint so tools can
    // attach to an existing Chromium/Chrome instance.  Empty string =
    // let the launcher spawn its own browser.  Upstream 64b35471.
    cfg["browser"] = {
        {"cdp_url", ""},
    };

    // --- TTS (scaffold) ---
    cfg["tts"] = {
        {"provider", "edge"},
    };

    // --- STT (v14 provider-keyed shape) ---
    cfg["stt"] = {
        {"enabled", true},
        {"provider", "local"},
        {"local", {{"model", "base"}, {"language", ""}}},
        {"openai", {{"model", "whisper-1"}}},
        {"mistral", {{"model", "voxtral-mini-latest"}}},
    };

    // --- Providers dict (v12 rename target, empty by default) ---
    cfg["providers"] = json::object();

    // --- IANA timezone (v5 — empty = server-local) ---
    cfg["timezone"] = "";

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

    // --- Approvals (v15): cron_mode controls how cron jobs handle
    // dangerous commands.  "deny" = block, let the agent find an
    // alternative.  "approve" = auto-approve (historical behaviour,
    // opt-in).  Upstream 762f7e97.
    cfg["approvals"] = {
        {"mode", "manual"},
        {"timeout", 60},
        {"cron_mode", "deny"},
    };

    // --- execute_code (v15): controls CWD + Python interpreter for
    // the execute_code tool.  "project" (default) tracks the session's
    // TERMINAL_CWD and the active virtualenv so project deps (pandas,
    // torch, …) resolve; "strict" runs in a staging tmpdir with
    // ``sys.executable`` for maximum isolation.  Upstream 285bb2b9.
    cfg["code_execution"] = {
        {"mode", "project"},
    };

    // --- Network (v15): force_ipv4 works around hosts where Python's
    // AAAA-first resolution hangs for the full TCP timeout before
    // falling back to IPv4.  Upstream 1ca9b197.
    cfg["network"] = {
        {"force_ipv4", false},
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
