// Depth-port helpers for ``tools/terminal_tool.py``.  These complement the
// broader helpers in ``terminal_helpers.hpp`` by porting behaviour that
// lives directly inside ``terminal_tool.py`` rather than a shared helper
// module — exit-code interpretation, the shell-aware sudo rewriter, the
// "safe command preview" used in error messages, and the small heuristics
// that decide whether a command wants real stdin instead of a PTY.
//
// All functions here are pure (no IO, no globals) so they can be unit
// tested without spawning subprocesses.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::tools::terminal::depth {

// ---- Safe command previews ----------------------------------------------

// Best-effort preview of a command for logging / error messages.  Replaces
// newlines with spaces and truncates with an ellipsis.  Matches
// ``_safe_command_preview``.
std::string safe_command_preview(std::string_view command,
                                 std::size_t limit = 200);

// ---- Shell tokenisation -------------------------------------------------

// Return ``true`` when ``token`` looks like a leading shell environment
// assignment: NAME=value where NAME matches ``[A-Za-z_][A-Za-z0-9_]*``.
// Matches ``_looks_like_env_assignment``.
bool looks_like_env_assignment(std::string_view token);

// Read one shell token preserving quoting and backslash-escapes, starting
// at ``start``.  Returns the token text and the next index past it.
// Matches ``_read_shell_token``.
std::pair<std::string, std::size_t> read_shell_token(std::string_view command,
                                                     std::size_t start);

// ---- Sudo rewriter ------------------------------------------------------

struct SudoRewriteResult {
    std::string command;
    bool found_sudo = false;
};

// Rewrite only real, unquoted ``sudo`` command words (not plain text
// occurrences inside strings) to ``sudo -S -p ''``.  Matches the
// behaviour of ``_rewrite_real_sudo_invocations`` including the env-
// assignment resumption so that ``FOO=bar sudo cmd`` rewrites correctly.
SudoRewriteResult rewrite_real_sudo_invocations(std::string_view command);

// ---- Exit-code interpretation ------------------------------------------

// For a known-command / exit-code pair, return the human-friendly note
// explaining that the non-zero exit is expected (e.g. ``grep`` returns 1
// when there are no matches).  Returns ``std::nullopt`` when no
// interpretation applies.  Mirrors ``_interpret_exit_code``.
std::optional<std::string> interpret_exit_code(std::string_view command,
                                               int exit_code);

// Split a pipeline string on ``|`` / ``||`` / ``&&`` / ``;`` and return
// the trimmed final segment.  Exposed for testing the splitting used
// by ``interpret_exit_code``.
std::string last_pipeline_segment(std::string_view command);

// Given a single pipeline segment, extract the base command name (first
// non-assignment word, with any directory prefix stripped).  Empty
// result means the segment was all env assignments.
std::string extract_base_command(std::string_view segment);

// ---- Stdin-vs-PTY heuristic --------------------------------------------

// Return ``true`` when the command expects piped stdin rather than a
// PTY.  Currently matches ``gh auth login --with-token`` like the
// Python code.  Matches ``_command_requires_pipe_stdin``.
bool command_requires_pipe_stdin(std::string_view command);

// ---- Disk-usage warning threshold --------------------------------------

struct DiskWarningInput {
    double total_gb = 0.0;
    double threshold_gb = 500.0;
    bool already_warned_today = false;
};

struct DiskWarningOutput {
    bool should_warn = false;
    std::string message;
};

// Decide whether to emit a disk-usage warning.  ``_check_disk_usage_warning``
// compares the total size of ``hermes-*`` scratch subdirs against the
// configured threshold and guards the warning with a once-per-day marker
// file — this is the pure decision layer.
DiskWarningOutput evaluate_disk_warning(const DiskWarningInput& in);

// ---- Foreground timeout clamp ------------------------------------------

// Clamp a caller-supplied timeout to ``[1, hard_cap]`` and return the
// effective value.  Negative / zero values fall back to ``default_value``.
// Matches the ``FOREGROUND_MAX_TIMEOUT`` policy.
int clamp_foreground_timeout(int requested, int default_value, int hard_cap);

// ---- Env var parsing ---------------------------------------------------

// Parse an ``int`` env value with a fallback.  Reproduces the defensive
// ``_parse_env_var`` helper in the Python tool.
int parse_env_int(const std::string& raw, int fallback);

// Parse a ``float`` env value with a fallback.
double parse_env_double(const std::string& raw, double fallback);

// ---- Command masking for logs ------------------------------------------

// Redact obvious secret arguments from a command before logging.  Matches
// patterns like ``--token=XXX``, ``--api-key XXX``, ``AWS_SECRET=XXX``.
// Returns the redacted command and the count of replacements.
struct MaskResult {
    std::string redacted;
    std::size_t replaced = 0;
};
MaskResult mask_secret_args(std::string_view command);

}  // namespace hermes::tools::terminal::depth
