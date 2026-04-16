#include "hermes/mcp_server/session.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <sstream>

namespace hermes::mcp_server {

// ---------------------------------------------------------------------------
// SseQueue
// ---------------------------------------------------------------------------

SseQueue::SseQueue(std::size_t max_pending) : max_pending_(max_pending) {}

void SseQueue::push(std::string frame) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        if (frames_.size() >= max_pending_ && !frames_.empty()) {
            // Drop the oldest queued frame — a stuck reader shouldn't
            // cause unbounded server growth.
            frames_.pop_front();
        }
        frames_.push_back(std::move(frame));
    }
    cv_.notify_one();
}

bool SseQueue::wait_and_pop(std::string& out,
                            std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, timeout, [&] {
            return shutdown_ || !frames_.empty();
        })) {
        return false;
    }
    if (shutdown_ && frames_.empty()) return false;
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void SseQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

bool SseQueue::is_shutdown() const {
    std::lock_guard<std::mutex> lk(mu_);
    return shutdown_;
}

std::size_t SseQueue::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return frames_.size();
}

// ---------------------------------------------------------------------------
// SessionStore
// ---------------------------------------------------------------------------

SessionStore::SessionStore(std::chrono::minutes ttl, std::size_t max_sessions)
    : ttl_(ttl), max_sessions_(max_sessions) {}

std::shared_ptr<McpSession> SessionStore::create() {
    auto session = std::make_shared<McpSession>();
    session->id = mint_uuid_v4();
    session->created_at = std::chrono::system_clock::now();
    session->last_seen = session->created_at;
    session->queue = std::make_shared<SseQueue>();

    std::unique_lock<std::shared_mutex> lk(mu_);
    // If at the cap, evict the single most-stale session first.
    if (sessions_.size() >= max_sessions_) {
        auto oldest = sessions_.end();
        auto oldest_ts = std::chrono::system_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->second->last_seen < oldest_ts) {
                oldest_ts = it->second->last_seen;
                oldest = it;
            }
        }
        if (oldest != sessions_.end()) {
            if (oldest->second->queue) oldest->second->queue->shutdown();
            sessions_.erase(oldest);
        }
    }
    sessions_.emplace(session->id, session);
    return session;
}

std::shared_ptr<McpSession> SessionStore::touch(std::string_view id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = sessions_.find(std::string(id));
    if (it == sessions_.end()) return nullptr;
    auto now = std::chrono::system_clock::now();
    if (now - it->second->last_seen > ttl_) {
        // Expired — drop while holding the write lock.
        if (it->second->queue) it->second->queue->shutdown();
        sessions_.erase(it);
        return nullptr;
    }
    it->second->last_seen = now;
    return it->second;
}

std::shared_ptr<McpSession> SessionStore::get(std::string_view id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = sessions_.find(std::string(id));
    if (it == sessions_.end()) return nullptr;
    return it->second;
}

bool SessionStore::drop(std::string_view id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = sessions_.find(std::string(id));
    if (it == sessions_.end()) return false;
    if (it->second->queue) it->second->queue->shutdown();
    sessions_.erase(it);
    return true;
}

std::size_t SessionStore::gc_expired() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    const auto now = std::chrono::system_clock::now();
    std::size_t evicted = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second->last_seen > ttl_) {
            if (it->second->queue) it->second->queue->shutdown();
            it = sessions_.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

std::size_t SessionStore::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return sessions_.size();
}

std::vector<std::string> SessionStore::list_ids() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_) out.push_back(kv.first);
    return out;
}

void SessionStore::set_ttl(std::chrono::minutes ttl) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    ttl_ = ttl;
}

std::chrono::minutes SessionStore::ttl() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return ttl_;
}

std::string SessionStore::mint_uuid_v4() {
    // Pull 16 bytes from std::random_device (backed by /dev/urandom on
    // Linux / BCryptGenRandom on Windows) and format per RFC 4122 §4.4.
    std::random_device rd;
    std::uint8_t b[16];
    for (auto& octet : b) {
        octet = static_cast<std::uint8_t>(rd() & 0xFFu);
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(buf, 36);
}

}  // namespace hermes::mcp_server
