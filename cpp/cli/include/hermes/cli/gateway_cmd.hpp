// C++17 port of hermes_cli/gateway.py — `hermes gateway` subcommand.
//
// The Python version bundles ~2700 LOC of systemd/launchd service
// management, manual run, supervision, and setup wizard.  This C++
// port covers the full operator-facing surface:
//
//   - `start|stop|restart|status|reconnect` — manage a running
//      gateway process (systemd --user service when available,
//      launchd on macOS, PID-file fallback otherwise).
//   - `logs [--follow] [--tail N] [--filter PAT] [--grep PAT]
//           [--level LVL] [--since TS] [--journald]`
//                                         — view gateway log tail
//      with optional regex/level/time filters and journald source.
//   - `install|uninstall`                 — provision/remove the
//      systemd --user (Linux) or launchd (macOS) service unit.
//   - `reload`                            — send SIGHUP for config
//      reload without full restart.
//   - `pids [--profile NAME]`             — print PIDs of gateway
//      processes, optionally scoped to a profile via env scan.
//   - `doctor`                            — run credential /
//      dependency checks (tokens, config file readable, log dir
//      writable, platform-specific daemon tools available).
//   - `profiles`                          — list profiles with
//      running gateways (multi-instance awareness).
//
// Like other ported modules, the interactive setup wizard path
// (`hermes gateway setup`) is deferred to the existing setup wizard.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::gateway_cmd {

// ---------------------------------------------------------------------------
// Top-level dispatch.
// ---------------------------------------------------------------------------

int run(int argc, char* argv[]);

// ---------------------------------------------------------------------------
// Subcommand handlers.
// ---------------------------------------------------------------------------
int cmd_start(const std::vector<std::string>& argv);
int cmd_stop(const std::vector<std::string>& argv);
int cmd_status(const std::vector<std::string>& argv);
int cmd_restart(const std::vector<std::string>& argv);
int cmd_reconnect(const std::vector<std::string>& argv);
int cmd_reload(const std::vector<std::string>& argv);
int cmd_logs(const std::vector<std::string>& argv);
int cmd_install(const std::vector<std::string>& argv);
int cmd_uninstall(const std::vector<std::string>& argv);
int cmd_pids(const std::vector<std::string>& argv);
int cmd_doctor(const std::vector<std::string>& argv);
int cmd_profiles(const std::vector<std::string>& argv);

// ---------------------------------------------------------------------------
// Service unit rendering — pure helpers for tests.
// ---------------------------------------------------------------------------

// Render a systemd user unit body.
std::string render_service_unit(const std::string& exec_path);

// Variant with explicit profile name. Produces a service with a
// `HERMES_PROFILE=<name>` Environment= directive and a matching
// Description suffix.
std::string render_service_unit_for_profile(const std::string& exec_path,
                                            const std::string& profile);

// Render a launchd plist for macOS. `label` is the service label
// (e.g. "com.hermes.gateway"); `exec_path` is the hermes binary.
std::string render_launchd_plist(const std::string& label,
                                 const std::string& exec_path);

// Render a launchd plist scoped to a profile (distinct Label + env).
std::string render_launchd_plist_for_profile(const std::string& exec_path,
                                             const std::string& profile);

// Service name helpers.
std::string systemd_service_name(const std::string& profile = "");
std::string launchd_label(const std::string& profile = "");

// ---------------------------------------------------------------------------
// Process discovery.
// ---------------------------------------------------------------------------

// Return the list of PIDs matching gateway-like processes.  Non-Linux
// platforms return empty.
std::vector<int> find_gateway_pids();

// Per-PID metadata used by `status`/`pids --verbose`.
struct GatewayProcess {
    int pid = 0;
    std::string cmdline;
    std::string profile;    // from HERMES_PROFILE env if readable; "" else
    std::string hermes_home;// from HERMES_HOME env if readable
    std::uint64_t rss_kb = 0;
    std::uint64_t uptime_sec = 0;
};

// Resolve full metadata for a PID (empty struct if unreadable).
GatewayProcess describe_process(int pid);

// Return the list of gateway processes grouped by profile.
std::vector<GatewayProcess> find_gateway_processes();

// Filter the list to a given profile name ("" = default / unnamed).
std::vector<GatewayProcess> filter_by_profile(
    const std::vector<GatewayProcess>& all,
    const std::string& profile);

// ---------------------------------------------------------------------------
// systemd / launchd introspection.
// ---------------------------------------------------------------------------

// Whether the systemd --user unit is installed and active.
// Returns false on non-Linux.
bool systemd_service_installed(const std::string& profile = "");
bool systemd_service_active(const std::string& profile = "");

// macOS launchd.
bool launchd_service_installed(const std::string& profile = "");
bool launchd_service_loaded(const std::string& profile = "");

// Returns true if systemctl is on PATH.
bool has_systemctl();
// Returns true if launchctl is on PATH.
bool has_launchctl();
// Returns true if journalctl is on PATH.
bool has_journalctl();

// ---------------------------------------------------------------------------
// Credential / config doctor checks.
// ---------------------------------------------------------------------------

enum class CheckStatus { OK, WARN, FAIL, SKIP };

struct DoctorResult {
    std::string name;
    CheckStatus status = CheckStatus::OK;
    std::string detail;
};

// List of platform credential env-vars the gateway might need.
// Returns pairs of {platform, env_var_name}.
std::vector<std::pair<std::string, std::string>> gateway_credential_env_vars();

// Run the full doctor checklist.  Results include credential presence
// per platform, log dir writeability, config file existence, daemon
// tool availability.
std::vector<DoctorResult> run_doctor_checks();

// Summarise a result list into an exit code (0 = all OK or only WARN/SKIP,
// 1 = any FAIL).
int doctor_exit_code(const std::vector<DoctorResult>& results);

// ---------------------------------------------------------------------------
// Log filtering — tail/grep/level helpers exposed for tests.
// ---------------------------------------------------------------------------

struct LogFilter {
    std::optional<std::string> grep;      // plain substring, case-sensitive
    std::optional<std::string> regex;     // std::regex ECMAScript
    std::optional<std::string> min_level; // DEBUG|INFO|WARN|WARNING|ERROR
    std::optional<std::string> since;     // "YYYY-MM-DD HH:MM:SS"
    bool invert = false;                  // match lines that do NOT match
};

// Apply a filter to a single log line; return true if it passes.
bool log_line_matches(const std::string& line, const LogFilter& filter);

// Given a collection of lines (a log file loaded into memory), return
// those passing the filter, capped by `max_lines` (0 = unlimited).
std::vector<std::string> filter_log_lines(
    const std::vector<std::string>& lines,
    const LogFilter& filter,
    std::size_t max_lines = 0);

// Parse a `LEVEL` at the start of a standardised log line. Returns
// uppercase level or empty string.
std::string parse_log_level(const std::string& line);

// Rank a level name (higher = more severe). Unknown names return 0.
int level_rank(const std::string& level);

// ---------------------------------------------------------------------------
// Argument parser — exposed for tests.
// ---------------------------------------------------------------------------

struct LogsOptions {
    bool follow = false;
    int tail = 50;
    bool journald = false;
    LogFilter filter;
};

LogsOptions parse_logs_args(const std::vector<std::string>& argv);

}  // namespace hermes::cli::gateway_cmd
