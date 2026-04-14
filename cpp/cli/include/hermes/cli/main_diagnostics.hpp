// main_diagnostics — startup banner + version dump + quick health probes.
//
// Ports Python's hermes_cli/banner.py + cmd_version() introspection so the
// C++ CLI can print a matching banner on startup and a matching
// `hermes version` / `hermes status --all` dump.
//
// Design goal: every probe here is a *read-only* fact gatherer.  Callers
// decide whether to display, log, or serialize the results.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::diagnostics {

// ---------------------------------------------------------------------------
// Versions & build info
// ---------------------------------------------------------------------------
struct BuildInfo {
    std::string hermes_version;   // "hermes-cpp 0.1.0"
    std::string hermes_release_date;
    std::string compiler;         // "gcc 13.3.0" / "clang 18.1.3"
    std::string build_type;       // "Release" | "Debug"
    std::string target_triple;    // "x86_64-linux-gnu"
    std::string build_date;       // compile-time __DATE__
    std::string git_commit;       // if available
    std::string libcurl_version;
    std::string sqlite_version;
    std::string openssl_version;
};

BuildInfo collect_build_info();
std::string format_build_info(const BuildInfo& bi);

// ---------------------------------------------------------------------------
// Runtime environment probe — paths, permissions, disk space.
// ---------------------------------------------------------------------------
struct EnvironmentInfo {
    std::string hermes_home;
    std::string active_profile;
    bool hermes_home_writable = false;
    bool env_file_exists = false;
    bool config_file_exists = false;
    bool has_curl_binary = false;
    bool has_git_binary = false;
    bool has_python_binary = false;
    std::string python_version;        // if detected
    std::string shell;                 // from $SHELL
    std::string term;                  // from $TERM
    std::string lang;                  // from $LANG
    std::uint64_t disk_free_bytes = 0; // on hermes_home
    bool is_tty = false;
    bool has_tmux = false;
    bool has_screen = false;
    std::string platform;              // "linux"|"darwin"|"windows"|"wsl"|…
};

EnvironmentInfo collect_environment();
std::string format_environment(const EnvironmentInfo& env);

// ---------------------------------------------------------------------------
// Startup banner — printed on interactive chat start.
// Mirrors hermes_cli/banner.py::render_banner().
// ---------------------------------------------------------------------------
struct BannerOptions {
    bool color = true;
    bool short_form = false;      // true => one-line banner
    bool show_update_hint = true;
    std::string greeting;         // override "Hermes —"
    int terminal_width = 80;
};

std::string render_banner(const BannerOptions& opts = {});

// One-line variant used by pipe mode / `--quiet`.
std::string render_oneline_banner();

// ---------------------------------------------------------------------------
// Update probe — checks if an upgrade is available by polling the GitHub
// release API.  Returns nullopt on error.  Mirrors
// hermes_cli/banner.py::check_for_updates().
//
// The integer returned is the number of commits the local install is behind
// the remote HEAD, 0 meaning "up to date".
// ---------------------------------------------------------------------------
struct UpdateCheckResult {
    bool checked = false;
    int commits_behind = 0;
    std::string latest_tag;
    std::string remote_url;
    std::string error;  // set if the probe failed
};

UpdateCheckResult check_for_updates(int timeout_secs = 3);
std::string format_update_hint(const UpdateCheckResult& r);

// ---------------------------------------------------------------------------
// Model availability probe — issues a low-cost probe to the configured
// provider to confirm reachability.  Does not spend tokens.
// ---------------------------------------------------------------------------
struct ModelProbeResult {
    bool reachable = false;
    std::string provider;
    std::string model;
    std::string status;       // human-readable summary
    int http_status = 0;      // non-zero on transport response
    double latency_ms = 0.0;
};

ModelProbeResult probe_configured_model(int timeout_secs = 5);
std::string format_model_probe(const ModelProbeResult& r);

// ---------------------------------------------------------------------------
// Skill sync probe — checks whether skill index is stale.
// ---------------------------------------------------------------------------
struct SkillSyncInfo {
    int total_skills = 0;
    int disabled_skills = 0;
    std::string index_path;
    std::time_t last_sync = 0;
    bool needs_sync = false;   // true if older than 24h
    std::string error;
};

SkillSyncInfo probe_skill_sync();
std::string format_skill_sync(const SkillSyncInfo& s);

// ---------------------------------------------------------------------------
// MCP health — quick liveness check of registered MCP servers.
// ---------------------------------------------------------------------------
struct McpHealth {
    std::string name;
    bool alive = false;
    std::string status;
    std::string command;
};

std::vector<McpHealth> probe_mcp_health();
std::string format_mcp_health(const std::vector<McpHealth>& entries);

// ---------------------------------------------------------------------------
// Aggregate startup diagnostic — used by `hermes status --all` and the
// startup banner path in the CLI.  Gathers all of the above into one view.
// ---------------------------------------------------------------------------
struct StartupReport {
    BuildInfo build;
    EnvironmentInfo env;
    UpdateCheckResult update;
    ModelProbeResult model;
    SkillSyncInfo skills;
    std::vector<McpHealth> mcps;
    std::chrono::milliseconds probe_duration{0};
};

StartupReport collect_startup_report(bool skip_network = false);
std::string format_startup_report(const StartupReport& r,
                                  bool verbose = false,
                                  bool color = true);

}  // namespace hermes::cli::diagnostics
