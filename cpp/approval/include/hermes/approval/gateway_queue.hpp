// GatewayApprovalQueue — async enqueue-and-wait queue used by the gateway
// frontend. Tool handlers run in the agent thread; they call enqueue_and_wait
// which:
//   1. invokes notify_cb (non-blocking — gateway sends a chat message)
//   2. blocks the caller on a condition variable until /approve, /deny,
//      /yolo, or a timeout arrives
//
// Resolutions are delivered via resolve() (called from the gateway slash
// command handler). cancel_session() wakes everything in a session with
// Denied — used by /stop or session shutdown.
#pragma once

#include "hermes/approval/session_state.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hermes::approval {

class GatewayApprovalQueue {
public:
    using NotifyCallback = std::function<void(const ApprovalRequest& req)>;

    // Enqueue a request and block until resolved.
    // Returns: Approved / Denied / Yolo / (Denied if timed out).
    // notify_cb is called BEFORE the wait — it must not block.
    ApprovalDecision enqueue_and_wait(
        const ApprovalRequest& req,
        const NotifyCallback& notify_cb,
        std::chrono::seconds timeout = std::chrono::seconds(300));

    // Resolve a specific pending request. No-op if not found.
    void resolve(const std::string& session_key,
                 const std::string& request_id,
                 ApprovalDecision decision);

    // Cancel all pending requests in a session (delivers Denied).
    void cancel_session(const std::string& session_key);

    // Number of pending requests in a session (testing aid).
    std::size_t pending_count(const std::string& session_key) const;

private:
    struct Pending {
        ApprovalRequest req;
        std::condition_variable cv;
        std::optional<ApprovalDecision> decision;
        bool cancelled = false;
    };

    mutable std::mutex mu_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::shared_ptr<Pending>>>
        pending_;
};

}  // namespace hermes::approval
