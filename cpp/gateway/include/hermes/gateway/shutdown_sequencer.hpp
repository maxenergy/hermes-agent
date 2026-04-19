// Graceful shutdown orchestrator for the gateway.
//
// Ports the multi-phase /restart and /shutdown flow from gateway/run.py:
//
//   1. Transition to "draining" — reject new work (or queue, per config)
//   2. Interrupt running agents (request_stop)
//   3. Wait up to ``agent_drain_timeout`` for agents to finish
//   4. Snapshot pending queues into persistent storage
//   5. Disconnect every adapter (reverse registration order)
//   6. Flush session store
//   7. Transition to "stopped"
//
// Each phase is time-budgeted independently so a single slow adapter
// cannot block the whole flow.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::gateway {

enum class ShutdownPhase {
    Idle,
    Draining,
    InterruptingAgents,
    WaitingForAgents,
    SnapshottingQueues,
    DisconnectingAdapters,
    FlushingSessions,
    Stopped,
};

const char* shutdown_phase_name(ShutdownPhase phase);

struct ShutdownBudget {
    std::chrono::milliseconds drain_grace{200};
    std::chrono::milliseconds agent_drain_timeout{15000};
    std::chrono::milliseconds per_adapter_timeout{3000};
    std::chrono::milliseconds session_flush_timeout{2000};
};

struct ShutdownOutcome {
    ShutdownPhase final_phase = ShutdownPhase::Idle;
    std::chrono::milliseconds total_elapsed{0};
    std::vector<std::string> slow_adapters;  // adapters that hit timeout
    std::vector<std::string> errors;
    bool agent_drain_timed_out = false;
    int agents_still_running = 0;
    // Count of stale session entries dropped during FlushingSessions
    // (upstream eb07c056).  Zero when no prune fn is registered.
    std::size_t stale_pruned = 0;
    // True if the session-close fn ran to completion (upstream 31e72764).
    bool session_closed = false;
};

// Registration-order list of "closers" — each closer is called during
// DisconnectingAdapters with up to ``per_adapter_timeout``.  Name is
// used in slow_adapters / error reporting.
struct AdapterCloser {
    std::string name;
    std::function<void()> fn;
};

class ShutdownSequencer {
public:
    using AgentStopFn = std::function<void()>;
    using PendingAgentCount = std::function<int()>;
    using QueueSnapshotFn = std::function<void()>;
    using SessionFlushFn = std::function<void()>;
    using SessionCloseFn = std::function<void()>;
    using StalePruneFn = std::function<std::size_t()>;
    using PhaseCallback = std::function<void(ShutdownPhase)>;

    ShutdownSequencer();

    void set_budget(ShutdownBudget budget);
    ShutdownBudget budget() const;

    void set_agent_stop(AgentStopFn fn);
    void set_agent_counter(PendingAgentCount fn);
    void set_queue_snapshot(QueueSnapshotFn fn);
    void set_session_flush(SessionFlushFn fn);
    // Upstream 31e72764: after FlushingSessions, explicitly close any
    // OS-level handles (SQLite WAL lock in Python; placeholder in C++)
    // so a --replace restart can open the same directory without
    // contention.  Runs within the session-flush timeout budget.
    void set_session_close(SessionCloseFn fn);
    // Upstream eb07c056: optional stale-entry prune invoked once during
    // the FlushingSessions phase.  Returns the count of pruned rows
    // for the caller to log; must be fast (seconds, not minutes) so a
    // shutdown isn't blocked.  Runs within the session-flush timeout.
    void set_stale_prune(StalePruneFn fn);
    void set_phase_callback(PhaseCallback cb);

    void add_adapter(AdapterCloser closer);
    void clear_adapters();
    std::size_t adapter_count() const;

    // Run the shutdown flow synchronously.  Returns when every phase
    // has completed or its budget has elapsed.
    ShutdownOutcome run(std::string_view reason = {});

    // Current phase (for observers).
    ShutdownPhase phase() const;

    // True when ``run`` is currently active.
    bool in_progress() const;

    // Reset to Idle (for tests / repeated runs).
    void reset();

private:
    mutable std::mutex mu_;
    ShutdownBudget budget_;
    AgentStopFn agent_stop_;
    PendingAgentCount agent_counter_;
    QueueSnapshotFn queue_snapshot_;
    SessionFlushFn session_flush_;
    SessionCloseFn session_close_;
    StalePruneFn stale_prune_;
    PhaseCallback phase_cb_;
    std::vector<AdapterCloser> adapters_;

    std::atomic<ShutdownPhase> phase_{ShutdownPhase::Idle};
    std::atomic<bool> in_progress_{false};

    void set_phase(ShutdownPhase phase);
};

}  // namespace hermes::gateway
