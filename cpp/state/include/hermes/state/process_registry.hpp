// ProcessRegistry — data layer for background-process tracking.
//
// Provides in-memory bookkeeping, rolling output buffer, watch-pattern
// rate limiter, and JSON checkpoint/restore. Process killing uses
// SIGTERM/SIGKILL on POSIX systems.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::state {

enum class ProcessState { Running, Exited, Killed };
enum class PidScope { Host, Sandbox };

struct ProcessSession {
    std::string id;           // uuid-ish
    std::string command;
    std::string task_id;
    std::string session_key;
    std::optional<int> pid;
    std::optional<int> exit_code;
    std::filesystem::path cwd;
    std::chrono::system_clock::time_point started_at{};
    std::chrono::system_clock::time_point updated_at{};
    std::chrono::system_clock::time_point ended_at{};
    ProcessState state = ProcessState::Running;
    PidScope pid_scope = PidScope::Host;
    bool detached = false;
    bool notify_on_complete = false;

    std::vector<std::string> watch_patterns;
    std::string output_buffer;  // rolling, trimmed to `output_buffer_max`
    std::size_t output_buffer_max = 200 * 1024;

    // Drop from the front of `output_buffer` as needed when appending
    // would exceed `output_buffer_max`. The tail of the buffer (most
    // recent output) is preserved.
    void append_output(std::string_view chunk);

    // Watch-pattern rate limit state. See ProcessRegistry::feed_output
    // for the 8-per-10s + 45s-sustained-overload policy.
    int notifications_in_window = 0;
    std::chrono::system_clock::time_point window_start{};
    std::chrono::system_clock::time_point overload_start{};
    bool overloaded() const;
};

struct WatchNotification {
    std::string process_id;
    std::string pattern;
    std::string line;
    std::chrono::system_clock::time_point at{};
    bool synthetic = false;  // true for the overload-kill notification
};

// Options for ProcessRegistry::spawn_local.
//
// Mirrors Python `process_registry.spawn_local()` semantics.  Empty fields
// take sensible defaults (cwd = current directory, env = inherit caller's
// environment, command run via `/bin/sh -c <command>` so shell features
// like pipes / redirection / expansion keep working).
struct SpawnOptions {
    std::string command;
    std::filesystem::path cwd;  // empty = current working directory
    std::string task_id;
    std::string session_key;
    std::vector<std::string> watch_patterns;

    // When non-empty, these KEY=VALUE pairs are merged onto the caller's
    // environment (caller values win for keys in env_vars).
    std::unordered_map<std::string, std::string> env_vars;

    // Hard deadline after which the child is SIGTERM'd (and then SIGKILL'd
    // if it doesn't exit within 2 seconds).  Zero = no timeout.
    std::chrono::seconds timeout{0};

    // Shell to invoke command under.  Defaults to "/bin/sh" which is
    // guaranteed present on POSIX systems.
    std::string shell = "/bin/sh";
};

class ProcessRegistry {
public:
    ProcessRegistry();
    explicit ProcessRegistry(const std::filesystem::path& checkpoint_path);
    ~ProcessRegistry();

    ProcessRegistry(const ProcessRegistry&) = delete;
    ProcessRegistry& operator=(const ProcessRegistry&) = delete;

    // Record an already-spawned process. Callers pass a fully-populated
    // ProcessSession with a pid (or none for test scenarios).
    std::string register_process(ProcessSession session);

    // Spawn a child process locally via fork/exec under `opts.shell -c`,
    // register the resulting session as Running, and start a background
    // reader thread that pipes the child's stdout+stderr into the
    // registry's rolling output buffer (via feed_output()).
    //
    // Returns the session id.  The child runs in its own process group
    // so kill() can signal the whole group.  When opts.timeout is
    // non-zero a watchdog thread enforces it: on timeout the session is
    // marked Killed with exit_code = 124.
    //
    // On fork/exec failure the session is marked Exited with
    // exit_code = -1 before the id is returned.
    //
    // POSIX-only.  Not implemented on Windows (the Windows agent will
    // fill that in separately).
    std::string spawn_local(const SpawnOptions& opts);

    std::optional<ProcessSession> get(const std::string& id) const;
    std::vector<ProcessSession> list_running() const;
    std::vector<ProcessSession> list_finished() const;

    void mark_exited(const std::string& id, int exit_code);
    // Mark the session state=Killed. Sends SIGTERM, waits 2s, then
    // SIGKILL if the process is still alive (POSIX only).
    void kill(const std::string& id);

    // Append `chunk` to the process's rolling output buffer, scan the
    // chunk line-by-line for watch_patterns, and enqueue matching
    // WatchNotifications subject to the 8/10s rate limit. If the rate
    // limit is exceeded for 45+ consecutive seconds the process is
    // killed and a synthetic "watch_pattern overload — process killed"
    // notification is enqueued.
    void feed_output(const std::string& id, std::string_view chunk);

    // Move all pending notifications out of the queue into the caller.
    std::vector<WatchNotification> drain_notifications();

    // Persist current state to `checkpoint_path` via atomic write.
    void checkpoint();
    // Reload from `checkpoint_path`. Previously-Running sessions are
    // stashed in the orphaned list instead of being re-added as
    // Running, giving gateway restart code a chance to reconcile them.
    void restore_from_checkpoint();
    std::vector<ProcessSession> orphaned() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hermes::state
