// ProcessRegistry — phase 2 data layer for background-process tracking.
//
// Phase 2 delivers the in-memory bookkeeping, rolling output buffer,
// watch-pattern rate limiter, and JSON checkpoint/restore. Actual process
// spawning, signal delivery, and reader threads are deferred to Phase 7
// (Boost.Process). Any integration point that would talk to the OS is
// marked with a TODO(phase-7) comment.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

class ProcessRegistry {
public:
    ProcessRegistry();
    explicit ProcessRegistry(const std::filesystem::path& checkpoint_path);
    ~ProcessRegistry();

    ProcessRegistry(const ProcessRegistry&) = delete;
    ProcessRegistry& operator=(const ProcessRegistry&) = delete;

    // Record an already-spawned process. Phase 7 will wire this up to
    // Boost.Process; today callers in tests pass a fully-populated
    // ProcessSession with a fake pid (or none at all).
    std::string register_process(ProcessSession session);

    std::optional<ProcessSession> get(const std::string& id) const;
    std::vector<ProcessSession> list_running() const;
    std::vector<ProcessSession> list_finished() const;

    void mark_exited(const std::string& id, int exit_code);
    // Mark the session state=Killed. No syscall is issued in phase 2 —
    // callers wire this to actual signal delivery during phase 7.
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
