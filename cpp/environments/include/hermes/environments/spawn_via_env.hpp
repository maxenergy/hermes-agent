// spawn_via_env — run a *background* command through any
// BaseEnvironment backend.
//
// The Python reference (tools/process_registry.py:spawn_via_env) wraps
// the user command in a shell sequence that:
//   1. mkdir -p <temp>
//   2. nohup bash -lc "<cmd>" > <log> 2>&1 &
//   3. echo $! > <pid_file>; cat <pid_file>
//   4. on completion writes the exit code to <exit_file>
//
// This C++ port performs the same dance via `BaseEnvironment::execute()`,
// so *any* backend that implements `execute` transparently supports
// background spawn — Docker, Modal, Daytona, Singularity, SSH, …
//
// The returned BackgroundHandle lets callers poll log contents / exit
// status on demand; it is NOT a ProcessRegistry session — feeding it
// into ProcessRegistry lives in cpp/state.
#pragma once

#include "hermes/environments/base.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace hermes::environments {

struct BackgroundHandle {
    // Sandbox-scoped PID (opaque to the host).  Empty on spawn failure.
    std::optional<int> pid;
    // Paths *inside the sandbox* used for log / pid / exit.
    std::string log_path;
    std::string pid_path;
    std::string exit_path;
    // Stable id used as the filename prefix — fed back into subsequent
    // poll / kill calls so remote state can be located.
    std::string handle_id;
    // stderr from the spawn bootstrap (empty on success).
    std::string error;
};

struct SpawnViaEnvOptions {
    // Remote directory where the log/pid/exit files are created.
    // Defaults to /tmp.
    std::string temp_dir = "/tmp";
    // Unique id — when empty one is generated from the current time.
    std::string handle_id;
    // How long to wait for the backend execute() call that bootstraps
    // the background process.
    std::chrono::seconds bootstrap_timeout{10};
    // Additional working directory for the backgrounded command.
    std::filesystem::path cwd;
    std::unordered_map<std::string, std::string> env_vars;
};

// Launch `command` in the background via `env`.  Does NOT wait for it
// to finish — the returned handle can be polled via subsequent
// env.execute("cat <log_path>") calls.
BackgroundHandle spawn_via_env(BaseEnvironment& env,
                               const std::string& command,
                               const SpawnViaEnvOptions& opts = {});

// Poll an earlier handle for completion.  Returns nullopt when the
// process is still running, otherwise the integer exit code.
std::optional<int> poll_background(BaseEnvironment& env,
                                   const BackgroundHandle& handle);

// Read the current tail of the background log (up to `max_bytes`).
std::string read_background_log(BaseEnvironment& env,
                                const BackgroundHandle& handle,
                                std::size_t max_bytes = 64 * 1024);

// Send SIGTERM (or SIGKILL with `force=true`) to the backgrounded PID.
// Returns true when the remote `kill` reports success.
bool kill_background(BaseEnvironment& env,
                     const BackgroundHandle& handle,
                     bool force = false);

}  // namespace hermes::environments
