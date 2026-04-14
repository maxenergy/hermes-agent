// Inbound message pipeline for the gateway.
//
// Ports the Python methods that process raw MessageEvents between
// adapter ingress and agent dispatch:
//
//   _handle_active_session_busy_message
//   _queue_or_replace_pending_event
//   _drain_active_agents
//   _interrupt_running_agents
//   _dequeue_pending_text
//   _build_media_placeholder (see gateway_helpers)
//   _inject_watch_notification
//   _enrich_message_with_vision / _enrich_message_with_transcription
//     (stubs — the actual vision/transcription passes live in the tools
//     layer; here we provide the event-mutation plumbing)
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>  // MessageEvent

namespace hermes::gateway {

// Decision returned from the busy-session pre-check.
enum class BusyDecision {
    // Not busy — caller should dispatch the message to the agent.
    Dispatch,
    // Busy but queued for the next turn — send the user a "queued"
    // acknowledgement and carry on.
    Queued,
    // Busy and gateway is draining (restart/stop) and queuing is off —
    // send the user a "not accepting" acknowledgement.
    Rejected,
};

struct BusyResult {
    BusyDecision decision = BusyDecision::Dispatch;
    std::string notice;   // Message to send back to the user (if any).
};

// Pending-message queue keyed by session_key, with coalescing rules that
// match the Python ``merge_pending_message_event`` helper:
//
//   - Two text events for the same session collapse into one, with the
//     later caption replacing the earlier (so "photo then edit" wins).
//   - Media events append to a rolling list of media urls.
//   - A command event displaces any queued non-command event.
class PendingQueue {
public:
    // Append or coalesce ``event`` into the queue for ``session_key``.
    // Returns true if the queue already contained a message and the new
    // event merged into it.
    bool enqueue(const std::string& session_key, MessageEvent event);

    // Pop the earliest pending event for ``session_key`` (nullopt if
    // none).  Mirrors adapter.get_pending_message.
    std::optional<MessageEvent> dequeue(const std::string& session_key);

    // Return, as a single text blob, what the agent should see when it
    // picks up the queued event: either the event's text, or — for
    // media-only events — a placeholder built by build_media_placeholder.
    // Mirrors _dequeue_pending_text.
    std::optional<std::string> dequeue_text(const std::string& session_key);

    // Peek at the earliest pending event without consuming it.
    std::optional<MessageEvent> peek(const std::string& session_key) const;

    // Remove every queued event for a session.
    void clear(const std::string& session_key);

    // Number of sessions currently holding at least one pending event.
    std::size_t session_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::deque<MessageEvent>> queues_;
};

// Pipeline-level policy the gateway runner applies on every inbound
// event.  Pure functions — no adapter or I/O side effects.
class MessagePipeline {
public:
    // ``queue_during_drain`` matches HERMES_GATEWAY_QUEUE_DURING_DRAIN
    // from the Python side.
    explicit MessagePipeline(PendingQueue* queue, bool queue_during_drain);

    // Decide what to do with an event for an already-busy session.
    //   ``is_draining``     — gateway is in /restart or /stop drain.
    //   ``action_gerund``   — "restarting" / "stopping" — included in
    //                          the user-facing notice.
    BusyResult evaluate_busy(const MessageEvent& event,
                              const std::string& session_key,
                              bool is_draining,
                              std::string_view action_gerund);

    // Emit a ``[SYSTEM: ...]`` watch-pattern notification event that
    // the caller can feed back into the main dispatch path.  Mirrors
    // _inject_watch_notification.
    MessageEvent synthesize_watch_notification(const std::string& text,
                                                 const SessionSource& source);

    // Drain helper: poll ``is_running_count`` until it reaches 0 or the
    // timeout expires.  Returns a tuple of (final_snapshot,
    // timed_out).  The ``maybe_update_status`` callback is invoked
    // at least once at start and once at end, and whenever the running
    // count changes or ~1s elapses.
    struct DrainStats {
        int initial_count = 0;
        int final_count = 0;
        bool timed_out = false;
        std::chrono::milliseconds elapsed{0};
    };

    using CountSampler = std::function<int()>;
    using StatusCallback = std::function<void(int active_count, bool force)>;

    DrainStats drain_active(CountSampler sampler, StatusCallback status,
                             std::chrono::milliseconds timeout);

private:
    PendingQueue* queue_;
    bool queue_during_drain_;
};

// Coalesce two events for the same session_key.  Returns the merged
// event.  The later event wins for text; media urls stack and dedupe.
MessageEvent merge_pending_message_event(const MessageEvent& existing,
                                           const MessageEvent& incoming);

}  // namespace hermes::gateway
