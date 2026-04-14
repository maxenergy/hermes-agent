// Bounded per-session queue for inbound message events.
//
// The Python side relies on adapter-specific queues inside each runtime;
// for the C++ port we centralize a single bounded queue so the gateway
// runner can apply uniform back-pressure across all platforms.
//
// Policy knobs mirror gateway/run.py:
//
//   max_per_session      — HERMES_GATEWAY_QUEUE_MAX_PER_SESSION
//   max_total            — HERMES_GATEWAY_QUEUE_MAX_TOTAL
//   overflow_policy      — DropOldest | DropNewest | Reject
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <hermes/gateway/gateway_runner.hpp>  // MessageEvent

namespace hermes::gateway {

enum class OverflowPolicy {
    DropOldest,    // evict the head of the per-session queue
    DropNewest,    // reject the new event silently
    Reject,        // reject and signal caller (returns NotAccepted)
};

enum class BackpressureResult {
    Accepted,
    Merged,         // coalesced into an existing queued event
    DroppedOldest,  // accepted after evicting older event
    DroppedNewest,  // dropped incoming event
    NotAccepted,    // reject policy + queue full
};

struct BackpressureConfig {
    std::size_t max_per_session = 16;
    std::size_t max_total = 1024;
    OverflowPolicy policy = OverflowPolicy::DropOldest;

    // When true, events for the same session coalesce via
    // merge_pending_message_event (text override + media stacking).
    bool coalesce = true;

    // Max wall-clock age before an event is considered "stale" and
    // dropped by the sweep.  Zero disables sweeping.
    std::chrono::seconds max_age{300};
};

class BoundedSessionQueue {
public:
    explicit BoundedSessionQueue(BackpressureConfig cfg = {});

    // Push an event for ``session_key``.  The return code reports which
    // branch of the policy was taken.  Thread-safe.
    BackpressureResult push(const std::string& session_key,
                             MessageEvent event);

    // Pop the earliest event for ``session_key`` (nullopt if empty).
    std::optional<MessageEvent> pop(const std::string& session_key);

    // Drain all events for ``session_key``, in FIFO order.
    std::vector<MessageEvent> drain(const std::string& session_key);

    // Peek at the next event without consuming it.
    std::optional<MessageEvent> peek(const std::string& session_key) const;

    // Aggregate counts.
    std::size_t session_count() const;
    std::size_t session_size(const std::string& session_key) const;
    std::size_t total_size() const;

    // Drop events older than ``cfg_.max_age`` from every session.
    // Returns the number of events dropped.
    std::size_t sweep_stale();

    // Replace the policy at runtime — the queue is not resized; the
    // new bounds are enforced on the next ``push``.
    void reconfigure(BackpressureConfig cfg);

    BackpressureConfig config() const;

    // Introspection helper for tests/diagnostics.
    struct Snapshot {
        std::string session_key;
        std::size_t size = 0;
        std::chrono::system_clock::time_point oldest{};
        std::chrono::system_clock::time_point newest{};
    };
    std::vector<Snapshot> snapshot_sessions() const;

    // Remove every queued event.
    void clear();

private:
    struct Entry {
        MessageEvent event;
        std::chrono::system_clock::time_point received_at;
    };

    mutable std::mutex mu_;
    BackpressureConfig cfg_;
    std::unordered_map<std::string, std::deque<Entry>> queues_;
    std::size_t total_ = 0;

    // Unlocked helpers.
    bool try_coalesce_locked(std::deque<Entry>& q, const MessageEvent& event);
    BackpressureResult handle_overflow_locked(const std::string& key,
                                                std::deque<Entry>& q,
                                                MessageEvent&& event);
};

}  // namespace hermes::gateway
