#include "hermes/llm/credential_pool.hpp"

namespace hermes::llm {

bool PooledCredential::is_expired(
    std::chrono::system_clock::time_point now) const {
    if (expires_at.time_since_epoch().count() == 0) {
        return false;  // never expires
    }
    return now >= expires_at;
}

bool PooledCredential::is_usable(
    std::chrono::system_clock::time_point now) const {
    if (api_key.empty()) return false;
    return !is_expired(now);
}

void CredentialPool::set_refresher(const std::string& provider,
                                   Refresher fn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fn) {
        refreshers_[provider] = std::move(fn);
    } else {
        refreshers_.erase(provider);
    }
}

void CredentialPool::store(const std::string& provider,
                           PooledCredential cred) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[provider] = std::move(cred);
}

std::optional<PooledCredential> CredentialPool::get(
    const std::string& provider) {
    Refresher refresher;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(provider);
        if (it != entries_.end() && it->second.is_usable()) {
            return it->second;
        }
        auto rit = refreshers_.find(provider);
        if (rit != refreshers_.end()) {
            refresher = rit->second;
        }
    }
    if (!refresher) {
        // No refresher — drop stale entry (if any) and report miss.
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(provider);
        if (it != entries_.end() && !it->second.is_usable()) {
            entries_.erase(it);
        }
        return std::nullopt;
    }
    // Invoke refresher outside the lock to avoid blocking other readers.
    auto fresh = refresher(provider);
    std::lock_guard<std::mutex> lock(mu_);
    if (fresh.has_value() && fresh->is_usable()) {
        entries_[provider] = *fresh;
        return fresh;
    }
    entries_.erase(provider);
    return std::nullopt;
}

void CredentialPool::invalidate(const std::string& provider) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(provider);
}

void CredentialPool::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
}

std::size_t CredentialPool::evict_expired(
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.is_expired(now)) {
            it = entries_.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

std::size_t CredentialPool::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

CredentialPool& CredentialPool::global() {
    static CredentialPool instance;
    return instance;
}

}  // namespace hermes::llm
