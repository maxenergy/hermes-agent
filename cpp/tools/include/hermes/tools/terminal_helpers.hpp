// Terminal helpers — extended utilities ported from tools/terminal_tool.py.
//
// Pure, side-effect-free building blocks the terminal tool uses to
// enforce safety policy, interpret commands, and forward environment
// variables.  Separated from terminal_tool.cpp so the test suite can
// exercise them without actually spawning processes.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::tools::terminal {

// ── Workdir safety ────────────────────────────────────────────────────

// Reject workdir values whose characters fall outside the allowlist
// (alphanumeric, path separators, and a short safe-punctuation set).
// Returns empty optional on safe, otherwise a human-readable error
// message naming the first offending character.
std::optional<std::string> validate_workdir_charset(std::string_view workdir);

// ── Command guards ────────────────────────────────────────────────────

// Classification of a raw shell command.  The terminal tool consults
// this table before executing anything to prompt for approval or
// outright block obviously destructive invocations.
enum class DangerLevel { Safe, Warn, Block };

struct DangerReport {
    DangerLevel level{DangerLevel::Safe};
    std::string reason;   // empty when level == Safe
    std::string category; // "rm-rf-root", "disk-fill", "fork-bomb", ...
};

// Scan |command| for obvious danger patterns.  Matches the heuristics
// in tools/approval.py::check_dangerous_command — rm -rf /, dd of=/dev/,
// :(){ :|:& };:, mkfs, chmod -R 777 /, curl | sh, etc.
DangerReport scan_dangerous_command(std::string_view command);

// Allow/deny list evaluation.  The terminal_tool.py supports a
// HERMES_TERMINAL_DENY / HERMES_TERMINAL_ALLOW env-var pair which is
// parsed into regex or glob patterns.  This helper takes pre-split
// pattern lists and returns the decision.
enum class AccessDecision { Allow, Deny, Unlisted };

AccessDecision evaluate_access(std::string_view command,
                               const std::vector<std::string>& allow,
                               const std::vector<std::string>& deny);

// ── Env var propagation ───────────────────────────────────────────────

// Policy for forwarding the host's environment into a subprocess.
// Mirrors the Python env_passthrough module: a set of explicitly-allowed
// names, a set of blocked prefixes (AWS_*, SSH_*, ...), and a wildcard
// flag to forward everything that isn't blocked.
struct EnvPassthroughPolicy {
    std::unordered_set<std::string> allow;        // exact names
    std::vector<std::string> allow_prefixes;      // prefix match
    std::unordered_set<std::string> block;        // exact names
    std::vector<std::string> block_prefixes;      // prefix match
    bool forward_all{false};                      // default: only allowlisted
};

// Filter |src| against the policy.  Returns the subset that should be
// forwarded to the subprocess.
std::unordered_map<std::string, std::string>
filter_env(const std::unordered_map<std::string, std::string>& src,
           const EnvPassthroughPolicy& policy);

// Parse the canonical colon-separated pattern list into a vector.
std::vector<std::string> split_pattern_list(std::string_view raw);

// ── Command categorization ────────────────────────────────────────────

// Derive a rough category label for the first significant command
// word.  Used for metrics and to route into different sandboxes.
//
// Returns one of: "git", "build", "pkg-manager", "network", "file",
// "process", "container", "shell-internal", "other".
std::string categorize_command(std::string_view command);

// ── Interactive mode detection ────────────────────────────────────────

// Return true when the command looks like it needs a real PTY —
// vim/nano/emacs/less/htop/top/etc.  The tool rejects these in
// non-PTY mode with a helpful tip.
bool needs_pty(std::string_view command);

// ── Signal forwarding ─────────────────────────────────────────────────

// Map a short signal name (case-insensitive) to its POSIX number.
// Returns -1 on unknown.
int signal_from_name(std::string_view name);

// Inverse lookup — signal number → canonical uppercase name
// ("SIGTERM"), or empty for unknown.
std::string signal_to_name(int signal);

// ── Interrupt / timeout heuristic ─────────────────────────────────────

// When a command times out, suggest how much higher the caller should
// set the timeout for a retry.  The heuristic looks at known
// long-running phases ("npm install", "apt update", "cmake --build")
// and returns a "soft recommendation" in seconds.  Returns 0 when no
// hint applies.
int suggested_retry_timeout(std::string_view command,
                            int original_timeout_sec);

// ── Sudo detection ────────────────────────────────────────────────────

// Decide whether a command — after it has already been rewritten by
// rewrite_real_sudo_invocations — will actually require a password.
// Looks for the "sudo -n" no-prompt flag and for "SUDO_ASKPASS=..."
// environment leaders.
bool sudo_requires_password(std::string_view rewritten_command);

// ── Output truncation ─────────────────────────────────────────────────

// Truncate a combined stdout/stderr buffer to at most |max_chars|,
// preserving head and tail slices with a middle-elided marker.  The
// Python tool uses (~60% head, ~40% tail) — we match that ratio.
std::string truncate_output(const std::string& combined,
                            std::size_t max_chars);

// ── Watch patterns ────────────────────────────────────────────────────

// Return the index of the first pattern that matches |text|, or -1 when
// none match.  Patterns are plain substring matches (case-sensitive).
int first_match(const std::string& text,
                const std::vector<std::string>& patterns);

// ── Retry / backoff for transient errors ──────────────────────────────

// Compute exponential backoff with jitter in the [base, base + jitter]
// range.  Used by the managed gateway client path.
std::chrono::milliseconds backoff_ms(int attempt, int base_ms = 200,
                                     int cap_ms = 4000);

// ── Process group / PID file management ───────────────────────────────

// Validate a caller-supplied PID: must be in (0, 2^22), not be one of
// the special PIDs (1 init, 2 kthreadd), and not be this process.
std::optional<std::string> validate_pid(int pid);

// ── Command line splitting ────────────────────────────────────────────

// Extract the first "bare" command word (the program name) from a
// shell command line, skipping leading env assignments.  Used for
// categorization and for matching deny/allow lists.  Returns empty when
// the command is all env assignments.
std::string first_program(std::string_view command);

// ── Working directory resolution ──────────────────────────────────────

// Expand leading "~" / "~user" into an absolute path.  Returns the
// canonical string when the expansion succeeds; otherwise returns the
// input unchanged.  Does not touch the filesystem.
std::string expand_user(std::string_view path);

// ── Command string utilities ──────────────────────────────────────────

// Strip all leading env assignments (e.g. "FOO=bar BAZ=qux ls -l" →
// "ls -l") while returning the removed assignments separately.  The
// Python version uses this to decide whether to pass the assignments
// to execvpe() instead of splicing them into the command string.
struct StripEnvResult {
    std::string remaining;
    std::vector<std::pair<std::string, std::string>> env;
};
StripEnvResult strip_leading_env(std::string_view command);

}  // namespace hermes::tools::terminal
