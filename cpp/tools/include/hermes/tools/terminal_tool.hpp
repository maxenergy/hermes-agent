// Terminal tools — terminal (foreground + background), process management.
// Registered in the "terminal" toolset via register_terminal_tools().
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "hermes/environments/base.hpp"

namespace hermes::tools {

void register_terminal_tools();

// ── Helper utilities ported from tools/terminal_tool.py ───────────────

namespace terminal {

// Return a safe, truncated preview of |command| for logs/errors.  Never
// leaks secrets or full payloads.  Limit defaults to 200 chars.
std::string safe_command_preview(std::string_view command,
                                 std::size_t limit = 200);

// Return true when |token| looks like a shell leading env assignment —
// e.g. "FOO=bar".  Does not accept tokens that start with "=" or that
// have a name containing non-ident chars.
bool looks_like_env_assignment(std::string_view token);

// Read one shell token starting at |start| in |command|, preserving
// single- and double-quoted strings and backslash escapes.  Returns
// (token, end_index).
std::pair<std::string, std::size_t> read_shell_token(std::string_view command,
                                                     std::size_t start);

// Rewrite bare `sudo` command-start words to `sudo -S -p ''` so the
// caller can feed a password via stdin.  Returns (transformed, found).
std::pair<std::string, bool> rewrite_real_sudo_invocations(
    std::string_view command);

// Interpret a non-zero exit code for a given |command|.  Returns a
// human-readable note when the exit code is "benign" (e.g. grep exit
// 1 = no matches) and an empty optional otherwise.
std::optional<std::string> interpret_exit_code(std::string_view command,
                                               int exit_code);

// Return true when PTY mode would break stdin-driven commands — e.g.
// `gh auth login --with-token` expects piped stdin, not a TTY.
bool command_requires_pipe_stdin(std::string_view command);

// Validate a workdir path — returns the normalized absolute path on
// success, or an error string on failure.
struct WorkdirResult {
    std::string path;   // normalized absolute path (success only)
    std::string error;  // non-empty on failure
};
WorkdirResult validate_workdir(std::string_view workdir);

// Clamp the requested timeout to the [1, 3600] range.
int clamp_timeout(int requested);

}  // namespace terminal

// Environment factory for terminal command execution.  Batch runners
// (and SWE evaluators) install a factory so each terminal invocation is
// routed through the task's isolated environment (docker / modal /
// singularity / ...).  When no factory is installed the terminal falls
// back to ``LocalEnvironment`` — the prior behaviour.
//
// The factory receives the value of ``ToolContext::extra["environment"]``
// (if set) and must return a fresh environment instance.  Returning
// nullptr causes the caller to use local.
using TerminalEnvFactory = std::function<
    std::unique_ptr<hermes::environments::BaseEnvironment>(
        const std::string& env_name)>;

/// Install a process-wide terminal environment factory.  Pass an empty
/// ``std::function`` to clear.
void set_terminal_env_factory(TerminalEnvFactory factory);

/// Resolve the current factory → produce an environment instance.
/// Never returns nullptr — falls back to LocalEnvironment.
std::unique_ptr<hermes::environments::BaseEnvironment>
resolve_terminal_env(const std::string& env_name);

}  // namespace hermes::tools
