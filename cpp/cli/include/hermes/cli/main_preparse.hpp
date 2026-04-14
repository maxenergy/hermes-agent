// main_preparse — argument pre-processing that must run before any module
// touches HERMES_HOME or reads config.  Ports the following Python helpers
// from hermes_cli/main.py:
//
//   _apply_profile_override()   → pre_parse_profile_override()
//   _has_any_provider_configured() → has_any_provider_configured()
//   _relative_time(ts)          → format_relative_time(ts)
//   _resolve_last_cli_session() → resolve_last_cli_session()
//   _resolve_session_by_name_or_id() → resolve_session_by_name_or_id()
//   _require_tty(cmd)           → require_tty(cmd)
//   _coalesce_session_name_args() → coalesce_session_name_args()
//
// These are the helpers that the Python CLI relies on *before* subcommand
// dispatch to normalize argv, probe env state, and format user-facing output.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::preparse {

// ---------------------------------------------------------------------------
// Profile override — mutates argv in place and may set HERMES_HOME env var.
//
// Recognizes the forms:
//   --profile <name>
//   -p <name>
//   --profile=<name>
//
// If no flag is present, consults <HERMES_HOME default>/active_profile.
//
// On success the flag is stripped from argv so the main dispatcher never
// sees it.  Returns true if a profile was resolved.  Returns false (with
// argv unchanged) if no profile is active.
//
// The Python analogue is `_apply_profile_override`; mirrors its precedence
// order — explicit flag beats sticky active_profile file beats default.
// ---------------------------------------------------------------------------
struct ProfileOverrideResult {
    bool applied = false;                 // a non-default profile took effect
    std::string profile_name;             // the resolved name, if any
    std::string hermes_home;              // resolved HERMES_HOME path, if any
    std::string source;                   // "flag" | "active_profile" | ""
};

ProfileOverrideResult pre_parse_profile_override(int& argc, char** argv);

// Strip the `--profile=NAME` / `-p NAME` forms from an argv-like vector.
// Returns the cleaned argv and, via `removed`, the name captured (if any).
// Exposed so tests can exercise argv manipulation independently.
std::vector<std::string> strip_profile_flag(const std::vector<std::string>& in,
                                            std::optional<std::string>* removed = nullptr);

// Resolve the HERMES_HOME directory for a given profile name.  Throws
// std::runtime_error if the profile doesn't exist.  Returns the absolute
// path as a string (matching Python's `resolve_profile_env`).
std::string resolve_profile_home(const std::string& name);

// ---------------------------------------------------------------------------
// Relative-time formatter — mirrors Python's `_relative_time(ts)`.
//
// Returns one of:
//   "?"               — ts is 0 or negative
//   "just now"        — <60s
//   "{n}m ago"        — <1h
//   "{n}h ago"        — <1d
//   "yesterday"       — <2d
//   "{n}d ago"        — <1w
//   "YYYY-MM-DD"      — older
// ---------------------------------------------------------------------------
std::string format_relative_time(std::time_t ts,
                                 std::time_t now = 0 /* 0 = current time */);

// ---------------------------------------------------------------------------
// Provider detection — mirrors `_has_any_provider_configured()`.
//
// Probes: environment variables, ~/.hermes/.env, auth.json, config.yaml.
// Returns true if at least one provider credential path is usable.
// ---------------------------------------------------------------------------
struct ProviderProbe {
    bool configured = false;
    std::string source;   // "env" | "env_file" | "config" | "auth_json" | ""
    std::string details;  // optional free-form detail (masked key / provider id)
};

ProviderProbe probe_any_provider_configured();
bool has_any_provider_configured();

// Known API-key environment variables used by the provider probe.
const std::vector<std::string>& provider_env_vars();

// ---------------------------------------------------------------------------
// Session resolution helpers — ports `_resolve_last_cli_session()` and
// `_resolve_session_by_name_or_id(name_or_id)`.
//
// Both return std::nullopt when no session matches.
// ---------------------------------------------------------------------------
std::optional<std::string> resolve_last_cli_session();
std::optional<std::string> resolve_session_by_name_or_id(const std::string& name_or_id);

// ---------------------------------------------------------------------------
// TTY guard — ports Python's `_require_tty(command_name)`.
//
// When stdin is not a terminal, prints a friendly error to stderr and
// returns `false`.  Callers should treat `false` as "exit with code 1".
// The Python version calls sys.exit(1) directly; the C++ contract keeps
// control flow in the caller so tests can capture the error message.
// ---------------------------------------------------------------------------
bool require_tty(const std::string& command_name, std::ostream* err = nullptr);

// Convenience overload for callers that just want an exit-code.
int require_tty_or_exit_code(const std::string& command_name);

// ---------------------------------------------------------------------------
// Multi-word session-name coalescing — ports `_coalesce_session_name_args()`.
//
// The Python CLI lets users type `hermes -c my project name` and coalesces
// the trailing tokens into a single argument.  The C++ port accepts the
// raw argv (as vector<string>) and returns a new vector with trailing
// bare-word arguments concatenated into one string after `-c`/`--continue`.
// ---------------------------------------------------------------------------
std::vector<std::string> coalesce_session_name_args(
    const std::vector<std::string>& argv);

// ---------------------------------------------------------------------------
// CLI-mode hint — returns "cli" if running interactively, "pipe" if stdin
// is not a TTY, "file" if `--input FILE` is present.
// ---------------------------------------------------------------------------
std::string detect_cli_mode(int argc, char** argv);

// Extract `--input FILE` / `-i FILE` from argv, mutating argv in place.
// Returns the resolved file path if found, else empty string.
std::string extract_input_file(int& argc, char** argv);

// ---------------------------------------------------------------------------
// Pre-parse --yolo / --worktree / --pass-session-id / --verbose global flags.
// Returns a struct summarizing them and mutates argv in place.  These are
// parsed before subcommand dispatch because they affect session setup.
// ---------------------------------------------------------------------------
struct GlobalFlags {
    bool yolo = false;
    bool worktree = false;
    bool pass_session_id = false;
    bool verbose = false;
    bool quiet = false;
    std::vector<std::string> skills;   // from --skills / -s (comma-split)
    std::string resume;                // --resume / -r
    std::string continue_name;         // --continue / -c (may be "1" for "most recent")
    bool continue_flag = false;        // --continue was seen
    std::string source;                // --source
    int max_turns = 0;                 // --max-turns (0 = not set)
};

GlobalFlags pre_parse_global_flags(int& argc, char** argv);

}  // namespace hermes::cli::preparse
