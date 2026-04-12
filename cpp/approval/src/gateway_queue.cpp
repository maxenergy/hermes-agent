#include "hermes/approval/gateway_queue.hpp"

#include <utility>

namespace hermes::approval {

ApprovalDecision GatewayApprovalQueue::enqueue_and_wait(
    const ApprovalRequest& req, const NotifyCallback& notify_cb,
    std::chrono::seconds timeout) {
    auto entry = std::make_shared<Pending>();
    entry->req = req;

    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_[req.session_key][req.request_id] = entry;
    }

    // Notify before blocking. Callback runs on the calling (agent) thread
    // and must not block — it should just push a chat message and return.
    if (notify_cb) {
        notify_cb(req);
    }

    std::unique_lock<std::mutex> lock(mu_);
    const bool got_decision = entry->cv.wait_for(
        lock, timeout, [&] { return entry->decision.has_value() || entry->cancelled; });

    // Always remove ourselves from the pending map on the way out.
    auto session_it = pending_.find(req.session_key);
    if (session_it != pending_.end()) {
        session_it->second.erase(req.request_id);
        if (session_it->second.empty()) {
            pending_.erase(session_it);
        }
    }

    if (!got_decision || entry->cancelled || !entry->decision.has_value()) {
        return ApprovalDecision::Denied;  // timeout / cancelled
    }
    return *entry->decision;
}

void GatewayApprovalQueue::resolve(const std::string& session_key,
                                   const std::string& request_id,
                                   ApprovalDecision decision) {
    std::shared_ptr<Pending> entry;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto session_it = pending_.find(session_key);
        if (session_it == pending_.end()) return;
        auto req_it = session_it->second.find(request_id);
        if (req_it == session_it->second.end()) return;
        entry = req_it->second;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        entry->decision = decision;
    }
    entry->cv.notify_all();
}

void GatewayApprovalQueue::cancel_session(const std::string& session_key) {
    std::vector<std::shared_ptr<Pending>> to_wake;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto session_it = pending_.find(session_key);
        if (session_it == pending_.end()) return;
        for (auto& [_, entry] : session_it->second) {
            entry->cancelled = true;
            entry->decision = ApprovalDecision::Denied;
            to_wake.push_back(entry);
        }
    }
    for (auto& entry : to_wake) {
        entry->cv.notify_all();
    }
}

std::size_t GatewayApprovalQueue::pending_count(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = pending_.find(session_key);
    if (it == pending_.end()) return 0;
    return it->second.size();
}

}  // namespace hermes::approval
