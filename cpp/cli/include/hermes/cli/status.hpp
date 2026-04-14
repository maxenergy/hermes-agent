// status — C++ port of hermes_cli/status.py.
//
// The status command is a read-only dashboard. This module exposes the
// individual formatting helpers so they can be unit-tested, plus a top-
// level renderer that prints the full output.
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <ostream>
#include <string>
#include <vector>

namespace hermes::cli::status {

// --- Low-level formatting ---------------------------------------------------

// Check mark character with optional ANSI colour.
std::string check_mark(bool ok, bool color = true);

// Redact an API key for display. Empty input → "(not set)"; short keys
// collapse to "***"; otherwise first-4 + "..." + last-4.
std::string redact_key(const std::string& key);

// Format an ISO-8601 timestamp into local time. Unknown/bad input →
// "(unknown)".
std::string format_iso_timestamp(const std::string& value);

// --- Config helpers ---------------------------------------------------------

// Read the configured default model from config.yaml-style JSON. Returns
// "(not set)" when no model is configured.
std::string configured_model_label(const nlohmann::json& config);

// Parse the effective provider from config + env. Mirrors the Python
// fallback chain.
std::string effective_provider_label(const nlohmann::json& config);

// Parse the effective terminal backend — env TERMINAL_ENV overrides
// config["terminal"]["backend"]; falls back to "local".
std::string effective_terminal_backend(const nlohmann::json& config);

// --- Sections ---------------------------------------------------------------

// List of (label, env_var) pairs for API-key rendering.
struct ApiKeyRow {
    std::string label;
    std::string env_var;
};
const std::vector<ApiKeyRow>& api_key_rows();

// List of (label, [env_vars]) — env_vars are tried in order; the first
// present one wins.
struct ApiKeyProvider {
    std::string label;
    std::vector<std::string> env_vars;
};
const std::vector<ApiKeyProvider>& api_key_providers();

// Messaging platforms — display label, token env, optional home-channel env.
struct PlatformRow {
    std::string label;
    std::string token_env;
    std::string home_env;  // may be empty
};
const std::vector<PlatformRow>& messaging_platforms();

// --- Counts -----------------------------------------------------------------

// Count active sessions by reading <HERMES_HOME>/sessions/sessions.json.
// Returns 0 on missing/broken file.
std::size_t count_active_sessions();

// Count jobs from <HERMES_HOME>/cron/jobs.json. Pass `enabled_only` to
// return only the enabled subset.
std::size_t count_cron_jobs(bool enabled_only = false);

// --- Renderers --------------------------------------------------------------

struct Options {
    bool show_all = false;  // --all: unredacted keys
    bool deep = false;      // --deep: network probes
    bool color = true;
};

// Top-level dispatch used by main_entry.cpp.
int cmd_status(std::ostream& out, const Options& opts);

// CLI dispatch wrapper. Parses argv flags (--all, --deep, --no-color).
int dispatch(int argc, char** argv);

// --- Section renderers (exposed for tests) ---------------------------------

void render_environment(std::ostream& out, const nlohmann::json& config,
                        const Options& opts);
void render_api_keys(std::ostream& out, const Options& opts);
void render_apikey_providers(std::ostream& out, const Options& opts);
void render_messaging_platforms(std::ostream& out, const Options& opts);
void render_terminal_backend(std::ostream& out, const nlohmann::json& config,
                             const Options& opts);
void render_session_summary(std::ostream& out, const Options& opts);
void render_cron_summary(std::ostream& out, const Options& opts);

}  // namespace hermes::cli::status
