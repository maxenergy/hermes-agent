#include <hermes/gateway/backpressure.hpp>

#include <hermes/gateway/message_pipeline.hpp>

#include <algorithm>

namespace hermes::gateway {

BoundedSessionQueue::BoundedSessionQueue(BackpressureConfig cfg)
    : cfg_(cfg) {}

BackpressureConfig BoundedSessionQueue::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

void BoundedSessionQueue::reconfigure(BackpressureConfig cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
}

bool BoundedSessionQueue::try_coalesce_locked(std::deque<Entry>& q,
                                                const MessageEvent& event) {
    if (!cfg_.coalesce || q.empty()) return false;
    q.back().event = merge_pending_message_event(q.back().event, event);
    q.back().received_at = std::chrono::system_clock::now();
    return true;
}

BackpressureResult BoundedSessionQueue::handle_overflow_locked(
    const std::string& key, std::deque<Entry>& q, MessageEvent&& event) {
    (void)key;
    switch (cfg_.policy) {
        case OverflowPolicy::DropOldest: {
            if (!q.empty()) {
                q.pop_front();
                --total_;
            }
            q.push_back({std::move(event), std::chrono::system_clock::now()});
            ++total_;
            return BackpressureResult::DroppedOldest;
        }
        case OverflowPolicy::DropNewest:
            return BackpressureResult::DroppedNewest;
        case OverflowPolicy::Reject:
        default:
            return BackpressureResult::NotAccepted;
    }
}

BackpressureResult BoundedSessionQueue::push(const std::string& session_key,
                                               MessageEvent event) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& q = queues_[session_key];

    // Coalesce attempt.
    if (try_coalesce_locked(q, event)) {
        return BackpressureResult::Merged;
    }

    // Enforce per-session bound.
    if (q.size() >= cfg_.max_per_session) {
        return handle_overflow_locked(session_key, q, std::move(event));
    }

    // Enforce global bound.
    if (total_ >= cfg_.max_total) {
        return handle_overflow_locked(session_key, q, std::move(event));
    }

    q.push_back({std::move(event), std::chrono::system_clock::now()});
    ++total_;
    return BackpressureResult::Accepted;
}

std::optional<MessageEvent> BoundedSessionQueue::pop(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end() || it->second.empty()) return std::nullopt;
    auto ev = std::move(it->second.front().event);
    it->second.pop_front();
    --total_;
    if (it->second.empty()) queues_.erase(it);
    return ev;
}

std::vector<MessageEvent> BoundedSessionQueue::drain(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end()) return {};
    std::vector<MessageEvent> out;
    out.reserve(it->second.size());
    for (auto& e : it->second) out.push_back(std::move(e.event));
    total_ -= it->second.size();
    queues_.erase(it);
    return out;
}

std::optional<MessageEvent> BoundedSessionQueue::peek(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end() || it->second.empty()) return std::nullopt;
    return it->second.front().event;
}

std::size_t BoundedSessionQueue::session_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queues_.size();
}

std::size_t BoundedSessionQueue::session_size(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = queues_.find(session_key);
    if (it == queues_.end()) return 0;
    return it->second.size();
}

std::size_t BoundedSessionQueue::total_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_;
}

std::size_t BoundedSessionQueue::sweep_stale() {
    std::lock_guard<std::mutex> lock(mu_);
    if (cfg_.max_age.count() <= 0) return 0;
    auto now = std::chrono::system_clock::now();
    std::size_t dropped = 0;
    for (auto it = queues_.begin(); it != queues_.end();) {
        auto& q = it->second;
        while (!q.empty() && now - q.front().received_at > cfg_.max_age) {
            q.pop_front();
            --total_;
            ++dropped;
        }
        if (q.empty()) it = queues_.erase(it);
        else ++it;
    }
    return dropped;
}

std::vector<BoundedSessionQueue::Snapshot>
BoundedSessionQueue::snapshot_sessions() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Snapshot> out;
    out.reserve(queues_.size());
    for (auto& [k, q] : queues_) {
        if (q.empty()) continue;
        Snapshot s;
        s.session_key = k;
        s.size = q.size();
        s.oldest = q.front().received_at;
        s.newest = q.back().received_at;
        out.push_back(std::move(s));
    }
    return out;
}

void BoundedSessionQueue::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    queues_.clear();
    total_ = 0;
}

}  // namespace hermes::gateway
