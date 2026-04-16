// Cross-platform subprocess runner.
//
// Provides a minimal "run a command, capture its output" primitive that
// works identically on POSIX (fork+execvp+pipe+waitpid) and Win32
// (CreateProcessW+CreatePipe+WaitForSingleObject).  Streaming / long-lived
// child processes (CDP browser, MCP stdio transport, process_registry's
// pool) are not covered here — those paths retain their native
// implementations behind `#ifdef _WIN32` guards because the semantics
// differ too much to unify cleanly.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hermes::core::platform {

struct SubprocessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out = false;
    // Empty when the child actually ran; populated with a human-readable
    // message when the spawn itself failed (fork/CreateProcess error).
    std::string spawn_error;
};

struct SubprocessOptions {
    // argv[0] is the program; absolute or on PATH.  Must be non-empty.
    std::vector<std::string> argv;

    // If non-empty, override the child's working directory.
    std::string cwd;

    // If non-empty, fed to the child's stdin before closing the pipe.
    std::string stdin_input;

    // Zero / negative = no timeout.  Otherwise SIGKILL / TerminateProcess
    // fires after this duration and `timed_out` is set.
    std::chrono::milliseconds timeout = std::chrono::milliseconds{0};

    // Optional extra env vars (KEY=VALUE); the current environment is
    // otherwise inherited.
    std::vector<std::string> extra_env;

    // Redirect stdout/stderr to /dev/null (or NUL) if true.  Both pipes
    // are still created on the parent side so the spawn_error path works.
    bool discard_output = false;
};

// Run a command, blocking until it exits or times out.
//
// Returns a SubprocessResult: on POSIX / Win32 where the primitive is
// implemented, `spawn_error` is empty on success.  On unsupported
// platforms, `spawn_error` is set and `exit_code` is -1.
SubprocessResult run_capture(const SubprocessOptions& opts);

// Convenience wrapper: argv + optional cwd only.
inline SubprocessResult run_capture(
    std::vector<std::string> argv, std::string cwd = {}) {
    SubprocessOptions o;
    o.argv = std::move(argv);
    o.cwd = std::move(cwd);
    return run_capture(o);
}

}  // namespace hermes::core::platform
