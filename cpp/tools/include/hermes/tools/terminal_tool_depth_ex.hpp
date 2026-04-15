// Extended depth-port helpers for ``tools/terminal_tool.py``.  The
// original terminal_tool_depth.hpp covers backend routing and command
// validation; this file ports additional pure helpers the Python
// implementation relies on: safe-command preview for logs, shell-token
// reading that preserves quotes/escapes, sudo rewriting, exit-code
// interpretation (grep/diff/find/curl/git semantics), PTY-breaks-pipe
// detection, and workdir validation.
//
// All helpers here are pure — they never touch the filesystem, spawn
// a subprocess, or read environment variables.  They exist so unit
// tests can pin the Python behaviour byte-for-byte.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::tools::terminal::depth_ex {

// ---- Command preview ----------------------------------------------------

// Produce a log-safe preview of a raw command value (string) truncated
// to ``limit`` chars.  Mirrors ``_safe_command_preview``.  Null /
// empty-string inputs return the sentinel ``"<None>"`` / ``""``
// respectively.
std::string safe_command_preview(std::optional<std::string_view> command,
                                 std::size_t limit = 200);

// ---- Shell token reading -----------------------------------------------

// Parse one shell token starting at ``start`` in ``command``, honouring
// single quotes (no escapes), double quotes (``\`` escapes inside), and
// backslash escapes outside quotes.  Stops at whitespace or any of
// ``;|&()``.  Returns the token text and the index of the first char
// past the token.  Mirrors ``_read_shell_token``.
std::pair<std::string, std::size_t> read_shell_token(std::string_view command,
                                                     std::size_t start);

// Return ``true`` when ``token`` looks like a leading ``VAR=value``
// shell env assignment.  Name must match ``[A-Za-z_][A-Za-z0-9_]*``.
// Matches ``_looks_like_env_assignment``.
bool looks_like_env_assignment(std::string_view token);

// Rewrite only the real unquoted ``sudo`` words at the start of each
// command phrase with ``sudo -S -p ''``.  Plain-text occurrences
// (inside quotes, inside comments, or after non-leading operators) are
// left alone.  Returns the rewritten command plus a flag indicating
// whether any rewrite happened.  Matches ``_rewrite_real_sudo_invocations``.
struct SudoRewriteResult {
    std::string command;
    bool rewrote = false;
};
SudoRewriteResult rewrite_real_sudo_invocations(std::string_view command);

// ---- Exit-code interpretation -------------------------------------------

// Given a command line and its non-zero exit code, return a short
// human-readable note when the exit code is a known "not-really-an-
// error" value (grep=1 no matches, diff=1 files differ, git=1 changes,
// curl=6/7/22/28, etc.).  Returns empty when no interpretation
// applies.  Mirrors ``_interpret_exit_code``.
std::string interpret_exit_code(std::string_view command, int exit_code);

// Split a command line on ``&&``, ``||``, ``|``, ``;`` and return the
// last segment trimmed.  Used by ``interpret_exit_code`` internally
// but exposed for tests.
std::string last_command_segment(std::string_view command);

// Extract the base command name from a segment — first non-env-
// assignment word, path-stripped (``/usr/bin/grep`` -> ``grep``).
std::string extract_base_command(std::string_view segment);

// ---- PTY / stdin detection ----------------------------------------------

// Return ``true`` when running ``command`` under a PTY would break
// stdin-driven consumers (``gh auth login --with-token``).  Matches
// ``_command_requires_pipe_stdin``.
bool command_requires_pipe_stdin(std::string_view command);

// ---- Workdir validation -------------------------------------------------

// Validate a working-directory string.  Returns an error message when
// the path is invalid (empty, contains null bytes, or contains a
// sequence that would allow shell-escape when joined), else empty.
// Matches the structural half of ``_validate_workdir``.
std::string validate_workdir(std::string_view workdir);

// ---- Env var parsing ----------------------------------------------------

// Parse an env var into an integer with a default fallback.  Accepts
// decimal digits only.  Returns ``default_value`` when ``raw`` is
// nullopt, empty, or unparseable.  Matches the Python int branch of
// ``_parse_env_var``.
int parse_env_int(std::optional<std::string_view> raw, int default_value);

// Parse an env var into a boolean.  ``true/1/yes/on`` (case-insensitive)
// are truthy; ``false/0/no/off`` are falsy; anything else returns
// ``default_value``.
bool parse_env_bool(std::optional<std::string_view> raw, bool default_value);

// ---- Env override registry ----------------------------------------------

// Merge a base env map with per-task overrides.  Overrides with empty
// string values unset the variable (return a map that omits the key).
// Keys present only in base are preserved.  Matches the merging logic
// used by ``register_task_env_overrides`` when building the child env.
std::unordered_map<std::string, std::string> merge_env_overrides(
    const std::unordered_map<std::string, std::string>& base,
    const std::unordered_map<std::string, std::string>& overrides);

// ---- Cleanup threshold helpers ------------------------------------------

// Classify an env's "age" relative to the cleanup lifetime.  Returns
// ``true`` when ``age_seconds >= lifetime_seconds``.  Pure — callers
// pass the clock.
bool env_is_expired(double age_seconds, double lifetime_seconds);

// Given a list of (task_id, age_seconds) pairs and a lifetime cutoff,
// return the ids that should be cleaned up.  Preserves insertion
// order.  Mirrors the filter step in ``_cleanup_inactive_envs``.
std::vector<std::string> select_expired_env_ids(
    const std::vector<std::pair<std::string, double>>& envs,
    double lifetime_seconds);

// ---- Disk usage warnings ------------------------------------------------

// Compose the "disk usage" warning message used by
// ``_check_disk_usage_warning`` when the /tmp dir is above the
// threshold.  Returns empty when below the threshold.  ``used_pct``
// is 0..100.
std::string format_disk_usage_warning(double used_pct,
                                      double threshold_pct = 90.0);

}  // namespace hermes::tools::terminal::depth_ex
