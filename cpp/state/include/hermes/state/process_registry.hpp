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

// Auto-restart policy for spawn_local().  When the child exits with a
// non-zero (or non-matching) code the waiter thread can relaunch it up
// to `max_restarts` times, with a fixed delay between attempts.
struct RestartPolicy {
    // Zero disables auto-restart (default).
    int max_restarts = 0;
    // Only restart on these exit codes (empty = restart on *any* non-zero).
    std::vector<int> restart_on_exit_codes;
    // Delay between restart attempts.
    std::chrono::milliseconds backoff{1000};
    // When true, a timeout-kill (exit_code 124) also counts as a
    // restartable failure.
    bool restart_on_timeout = false;
};

// Per-process resource limits applied via POSIX setrlimit() in the
// child before execve.  Zero = inherit from parent.
struct ResourceLimits {
    // Max CPU seconds (RLIMIT_CPU).
    unsigned long cpu_seconds = 0;
    // Max address-space size in bytes (RLIMIT_AS).
    unsigned long memory_bytes = 0;
    // Max open file descriptors (RLIMIT_NOFILE).
    unsigned long max_open_files = 0;
    // Max core file size in bytes (RLIMIT_CORE).  Default on many
    // distros is "unlimited"; setting to 0 disables core dumps.
    bool disable_core_dumps = false;
};

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

    // Split stdout/stderr tails — maintained alongside `output_buffer`
    // so callers that care about the stream distinction can read just
    // one side.  Each is trimmed to `stream_buffer_max`.
    std::string stdout_buffer;
    std::string stderr_buffer;
    std::size_t stream_buffer_max = 100 * 1024;

    // Number of bytes persisted to SessionDB so far — used by
    // `persist_tail()` to only flush new bytes.
    std::size_t persisted_bytes = 0;

    // Restart bookkeeping.  Only populated when a RestartPolicy was
    // configured on the spawn options.
    int restart_attempts = 0;

    // Drop from the front of `output_buffer` as needed when appending
    // would exceed `output_buffer_max`. The tail of the buffer (most
    // recent output) is preserved.
    void append_output(std::string_view chunk);

    // Same, but route into the per-stream buffer as well.  stream = 1
    // for stdout, 2 for stderr.  Chunks are appended to both the
    // combined output_buffer and the stream-specific tail.
    void append_stream(std::string_view chunk, int stream);

    // Return the last `n` bytes of a given stream (stream=0 combined,
    // 1=stdout, 2=stderr).
    std::string tail(int stream, std::size_t n) const;

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

    // Optional auto-restart configuration.  Ignored when
    // max_restarts == 0.
    RestartPolicy restart;

    // Optional resource limits (POSIX setrlimit).  Zero values leave
    // the corresponding limit inherited from the parent.
    ResourceLimits limits;

    // When non-null, invoked on every output chunk the reader thread
    // pulls off the pipe.  Called with (process_id, stream, chunk)
    // where stream is 1 for stdout and 2 for stderr.  Thrown
    // exceptions are swallowed.
    std::function<void(const std::string&, int, std::string_view)>
        persist_sink;
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

    // Stream-aware variant.  stream=1 (stdout) / 2 (stderr).  Routes
    // into the stream-specific tail in addition to the combined buffer.
    void feed_output_stream(const std::string& id, std::string_view chunk,
                            int stream);

    // Return the tail of the given stream for process `id`.  stream=0
    // returns the combined buffer.  Empty string when no such
    // process.
    std::string tail(const std::string& id, int stream,
                     std::size_t n) const;

    // Drain any new bytes from `output_buffer` into `sink`.  Returns
    // the number of bytes flushed.  Used by the gateway to persist
    // output into SessionDB after each poll without re-writing older
    // bytes.
    std::size_t persist_tail(
        const std::string& id,
        const std::function<void(std::string_view)>& sink);

    // Register a process for auto-restart bookkeeping.  The waiter
    // thread consults this policy when a child exits; callers that
    // manage their own spawn path can apply it by calling
    // maybe_restart() from their waitpid handler.  Returns true if the
    // process was (or would be) restarted.
    bool maybe_restart(const std::string& id, int exit_code);
    void set_restart_policy(const std::string& id, RestartPolicy policy);
    RestartPolicy restart_policy(const std::string& id) const;

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
