// Hermes doctor — comprehensive diagnostic suite ported function-by-function
// from hermes_cli/doctor.py (Python reference, 1019 LoC).
//
// The doctor runs a set of diagnostic checks, prints colored PASS/WARN/FAIL
// rows, accumulates remediation hints, and (optionally) attempts auto-repair
// for fixable issues (`--fix`).
//
// Design notes:
//   * Each `DoctorCheck` category in the Python source maps to a free
//     function here.  The checks are aggregated by `run_doctor()` which is
//     the public entry point called from `main_entry::cmd_doctor()`.
//   * Every check updates a `DoctorReport` so that the caller can render
//     a summary either in human form or as JSON (`--json`).
//   * Side-effects (mutating the filesystem, launching child processes)
//     are isolated to concrete helper functions so unit tests can verify
//     the pure-logic pieces without touching the real `$HERMES_HOME`.
//
// Unlike the Python version, `run_doctor()` is designed to be re-entrant
// and does not rely on global state beyond the environment variables that
// `hermes::core::path::get_hermes_home()` consults.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::llm { class HttpTransport; }

namespace hermes::cli::doctor {

// Severity levels mirror the `check_ok / check_warn / check_fail` helpers
// from the Python reference.
enum class Severity { Ok, Warn, Fail, Info };

// A single row emitted during diagnostics.
struct Row {
    std::string category;  // e.g. "Python Environment", "Configuration Files"
    Severity severity = Severity::Ok;
    std::string label;     // short human-readable name
    std::string detail;    // optional trailing detail
    std::string fix_hint;  // optional remediation suggestion appended to summary
    bool auto_fixed = false;  // true if --fix repaired this issue
};

// Options that steer the run.
struct Options {
    bool fix = false;           // attempt auto-repair where possible
    bool json = false;          // emit machine output instead of colored text
    bool color = true;          // disabled automatically for non-TTY
    // Optional HTTP transport override — used by tests to stub out network
    // probes.  nullptr means "use the default (curl) transport".
    hermes::llm::HttpTransport* transport = nullptr;
    // Optional override for HERMES_HOME (used by tests to avoid polluting
    // the real home directory).  Empty = use get_hermes_home().
    std::filesystem::path home_override;
};

// Aggregated diagnostic report.
struct Report {
    std::vector<Row> rows;
    std::vector<std::string> issues;          // auto-fixable, remain after run
    std::vector<std::string> manual_issues;   // require human intervention
    int fixed_count = 0;
    int ok_count = 0;
    int warn_count = 0;
    int fail_count = 0;

    bool any_failures() const { return fail_count > 0; }
    int exit_code() const { return any_failures() ? 1 : 0; }
    std::string to_json() const;
};

// ── Individual check functions ──────────────────────────────────────────
//
// Each returns void and appends rows to the supplied report.  Helpers are
// exposed in the header so the test file can exercise them directly.

void check_runtime_environment(Report& r, const Options& opts);
void check_config_files(Report& r, const Options& opts);
void check_credentials(Report& r, const Options& opts);
void check_directory_structure(Report& r, const Options& opts);
void check_session_db(Report& r, const Options& opts);
void check_skill_index(Report& r, const Options& opts);
void check_plugin_load(Report& r, const Options& opts);
void check_gateway_lock(Report& r, const Options& opts);
void check_external_tools(Report& r, const Options& opts);
void check_api_reachability(Report& r, const Options& opts);
void check_memory_backend(Report& r, const Options& opts);
void check_mcp_servers(Report& r, const Options& opts);
void check_disk_space(Report& r, const Options& opts);
void check_file_permissions(Report& r, const Options& opts);
void check_terminal_capability(Report& r, const Options& opts);

// ── Utility helpers (pure — tested directly) ────────────────────────────

// Mirror of Python's `_has_provider_env_config(content)`.
bool has_provider_env_config(const std::string& env_contents);

// Classifies HTTP status into a Severity + detail message.
// 200 → Ok,  401 → Fail (invalid key),  4xx/5xx → Warn, anything else → Warn.
Severity classify_http_status(int status_code, std::string& detail_out);

// Returns true when the binary `cmd` is on $PATH (emulates `shutil.which`).
bool binary_on_path(const std::string& cmd);

// Returns available bytes on the filesystem containing `path`, or 0 on error.
std::uintmax_t available_disk_bytes(const std::filesystem::path& path);

// Checks that a file is mode 0600 (owner read/write only).  On Windows this
// unconditionally returns true (POSIX perms are not meaningful).
bool is_mode_0600(const std::filesystem::path& path);

// Attempts to chmod `path` to 0600.  Returns false on failure.
bool chmod_0600(const std::filesystem::path& path);

// Probes SQLite FTS5 availability by running a tiny in-memory query.  Does
// not require the `sqlite3` CLI binary — uses the linked SQLite library via
// hermes::state::SessionDB's underlying driver.
bool fts5_available();

// Runs `sqlite3_exec("PRAGMA integrity_check")` against `db_path`.  Returns
// empty string on success ("ok") or an error message.
std::string sqlite_integrity_check(const std::filesystem::path& db_path);

// Attempts to launch `mcp_server/main` (or equivalent child process) and
// tears it down immediately.  Returns {success, message}.
std::pair<bool, std::string> probe_mcp_child(const std::string& server_cmd);

// Parses `~/.hermes/skills/.hub/lock.json` and returns the installed count
// or -1 on parse error.  Wraps the Python "Skills Hub" check.
int read_skills_hub_lock(const std::filesystem::path& hub_dir);

// Heuristic terminal capability detection — returns a colon-separated
// capabilities list such as "tty:color:unicode".
std::string detect_terminal_capabilities();

// Gateway lock file state.  `lock_path` is typically
// `<HERMES_HOME>/gateway.lock`.  Returns one of:
//   "missing" — no lock file.
//   "running:<pid>" — pid is currently alive.
//   "stale:<pid>" — pid no longer alive (lock should be removed).
//   "unreadable" — file exists but could not be parsed.
std::string gateway_lock_state(const std::filesystem::path& lock_path);

// ── Top-level entry point ───────────────────────────────────────────────

// Runs all checks and returns the completed report.  When `opts.fix` is
// true, auto-fixable issues are repaired inline and `report.fixed_count`
// is incremented accordingly.
Report run_all(const Options& opts);

// Renders `report` to stdout in the colored human format (default) or JSON
// when `opts.json` is true.
void render(const Report& report, const Options& opts);

// Convenience dispatch invoked from `cmd_doctor()` — parses argv for the
// `--fix` and `--json` flags, runs all checks, renders, and returns the
// appropriate process exit code.
int run(int argc, char* argv[]);

}  // namespace hermes::cli::doctor
