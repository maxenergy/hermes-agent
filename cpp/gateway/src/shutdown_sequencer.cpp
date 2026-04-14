#include <hermes/gateway/shutdown_sequencer.hpp>

#include <future>
#include <thread>

namespace hermes::gateway {

const char* shutdown_phase_name(ShutdownPhase phase) {
    switch (phase) {
        case ShutdownPhase::Idle: return "idle";
        case ShutdownPhase::Draining: return "draining";
        case ShutdownPhase::InterruptingAgents: return "interrupting-agents";
        case ShutdownPhase::WaitingForAgents: return "waiting-for-agents";
        case ShutdownPhase::SnapshottingQueues: return "snapshotting-queues";
        case ShutdownPhase::DisconnectingAdapters:
            return "disconnecting-adapters";
        case ShutdownPhase::FlushingSessions: return "flushing-sessions";
        case ShutdownPhase::Stopped: return "stopped";
    }
    return "unknown";
}

ShutdownSequencer::ShutdownSequencer() = default;

void ShutdownSequencer::set_budget(ShutdownBudget budget) {
    std::lock_guard<std::mutex> lock(mu_);
    budget_ = budget;
}

ShutdownBudget ShutdownSequencer::budget() const {
    std::lock_guard<std::mutex> lock(mu_);
    return budget_;
}

void ShutdownSequencer::set_agent_stop(AgentStopFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    agent_stop_ = std::move(fn);
}

void ShutdownSequencer::set_agent_counter(PendingAgentCount fn) {
    std::lock_guard<std::mutex> lock(mu_);
    agent_counter_ = std::move(fn);
}

void ShutdownSequencer::set_queue_snapshot(QueueSnapshotFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_snapshot_ = std::move(fn);
}

void ShutdownSequencer::set_session_flush(SessionFlushFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    session_flush_ = std::move(fn);
}

void ShutdownSequencer::set_phase_callback(PhaseCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    phase_cb_ = std::move(cb);
}

void ShutdownSequencer::add_adapter(AdapterCloser closer) {
    std::lock_guard<std::mutex> lock(mu_);
    adapters_.push_back(std::move(closer));
}

void ShutdownSequencer::clear_adapters() {
    std::lock_guard<std::mutex> lock(mu_);
    adapters_.clear();
}

std::size_t ShutdownSequencer::adapter_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return adapters_.size();
}

void ShutdownSequencer::set_phase(ShutdownPhase phase) {
    phase_.store(phase, std::memory_order_release);
    PhaseCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = phase_cb_;
    }
    if (cb) {
        try { cb(phase); } catch (...) {}
    }
}

ShutdownPhase ShutdownSequencer::phase() const {
    return phase_.load(std::memory_order_acquire);
}

bool ShutdownSequencer::in_progress() const {
    return in_progress_.load(std::memory_order_acquire);
}

void ShutdownSequencer::reset() {
    in_progress_.store(false, std::memory_order_release);
    phase_.store(ShutdownPhase::Idle, std::memory_order_release);
}

ShutdownOutcome ShutdownSequencer::run(std::string_view reason) {
    (void)reason;
    ShutdownOutcome out;
    bool expected = false;
    if (!in_progress_.compare_exchange_strong(expected, true)) {
        out.final_phase = phase_.load();
        out.errors.emplace_back("shutdown already in progress");
        return out;
    }
    auto started = std::chrono::steady_clock::now();

    // --- Phase: Draining ---
    set_phase(ShutdownPhase::Draining);
    {
        ShutdownBudget b = budget();
        std::this_thread::sleep_for(b.drain_grace);
    }

    // --- Phase: InterruptingAgents ---
    set_phase(ShutdownPhase::InterruptingAgents);
    AgentStopFn stop_fn;
    PendingAgentCount counter_fn;
    QueueSnapshotFn snap_fn;
    SessionFlushFn flush_fn;
    std::vector<AdapterCloser> adapters;
    ShutdownBudget b;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_fn = agent_stop_;
        counter_fn = agent_counter_;
        snap_fn = queue_snapshot_;
        flush_fn = session_flush_;
        adapters = adapters_;
        b = budget_;
    }
    if (stop_fn) {
        try { stop_fn(); } catch (const std::exception& e) {
            out.errors.emplace_back(std::string("agent_stop: ") + e.what());
        } catch (...) {
            out.errors.emplace_back("agent_stop: unknown error");
        }
    }

    // --- Phase: WaitingForAgents ---
    set_phase(ShutdownPhase::WaitingForAgents);
    auto deadline = std::chrono::steady_clock::now() + b.agent_drain_timeout;
    while (counter_fn) {
        int count = 0;
        try { count = counter_fn(); } catch (...) { count = 0; }
        if (count <= 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            out.agent_drain_timed_out = true;
            out.agents_still_running = count;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // --- Phase: SnapshottingQueues ---
    set_phase(ShutdownPhase::SnapshottingQueues);
    if (snap_fn) {
        try { snap_fn(); } catch (const std::exception& e) {
            out.errors.emplace_back(std::string("queue_snapshot: ") + e.what());
        } catch (...) {
            out.errors.emplace_back("queue_snapshot: unknown error");
        }
    }

    // --- Phase: DisconnectingAdapters ---
    set_phase(ShutdownPhase::DisconnectingAdapters);
    // Reverse order — last registered shuts down first.
    for (auto it = adapters.rbegin(); it != adapters.rend(); ++it) {
        auto& closer = *it;
        if (!closer.fn) continue;
        // Run with timeout.
        auto fut = std::async(std::launch::async, [&] {
            try { closer.fn(); } catch (...) {}
        });
        if (fut.wait_for(b.per_adapter_timeout) != std::future_status::ready) {
            out.slow_adapters.push_back(closer.name);
            // Detach by letting the future's thread run on — best effort.
        }
    }

    // --- Phase: FlushingSessions ---
    set_phase(ShutdownPhase::FlushingSessions);
    if (flush_fn) {
        auto fut = std::async(std::launch::async, [&] {
            try { flush_fn(); } catch (const std::exception& e) {
                out.errors.emplace_back(std::string("session_flush: ") +
                                         e.what());
            } catch (...) {
                out.errors.emplace_back("session_flush: unknown error");
            }
        });
        if (fut.wait_for(b.session_flush_timeout) !=
            std::future_status::ready) {
            out.errors.emplace_back("session_flush: timeout");
        }
    }

    // --- Phase: Stopped ---
    set_phase(ShutdownPhase::Stopped);
    out.final_phase = ShutdownPhase::Stopped;
    out.total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    in_progress_.store(false, std::memory_order_release);
    return out;
}

}  // namespace hermes::gateway
