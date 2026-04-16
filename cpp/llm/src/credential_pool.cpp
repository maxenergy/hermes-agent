#include "hermes/llm/credential_pool.hpp"

#include <algorithm>

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
    Slot s;
    s.creds.push_back(std::move(cred));
    slots_[provider] = std::move(s);
}

void CredentialPool::add(const std::string& provider,
                         PooledCredential cred) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& slot = slots_[provider];
    for (const auto& existing : slot.creds) {
        if (existing.api_key == cred.api_key &&
            existing.base_url == cred.base_url) {
            return;  // dedupe
        }
    }
    slot.creds.push_back(std::move(cred));
}

std::size_t CredentialPool::count(const std::string& provider) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = slots_.find(provider);
    return it == slots_.end() ? 0 : it->second.creds.size();
}

std::optional<PooledCredential> CredentialPool::get(
    const std::string& provider) {
    Refresher refresher;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = slots_.find(provider);
        if (it != slots_.end() && !it->second.creds.empty()) {
            auto& slot = it->second;
            // Drop expired entries in-place so round-robin only sees
            // usable credentials.
            slot.creds.erase(
                std::remove_if(slot.creds.begin(), slot.creds.end(),
                               [](const PooledCredential& c) {
                                   return !c.is_usable();
                               }),
                slot.creds.end());
            if (!slot.creds.empty()) {
                auto idx = slot.cursor % slot.creds.size();
                ++slot.cursor;
                return slot.creds[idx];
            }
        }
        auto rit = refreshers_.find(provider);
        if (rit != refreshers_.end()) {
            refresher = rit->second;
        }
    }
    if (!refresher) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = slots_.find(provider);
        if (it != slots_.end() && it->second.creds.empty()) {
            slots_.erase(it);
        }
        return std::nullopt;
    }
    auto fresh = refresher(provider);
    std::lock_guard<std::mutex> lock(mu_);
    if (fresh.has_value() && fresh->is_usable()) {
        auto& slot = slots_[provider];
        // Append without duplicating an already-present key.
        bool dup = false;
        for (const auto& existing : slot.creds) {
            if (existing.api_key == fresh->api_key &&
                existing.base_url == fresh->base_url) {
                dup = true;
                break;
            }
        }
        if (!dup) slot.creds.push_back(*fresh);
        if (slot.creds.empty()) return std::nullopt;
        auto idx = slot.cursor % slot.creds.size();
        ++slot.cursor;
        return slot.creds[idx];
    }
    slots_.erase(provider);
    return std::nullopt;
}

void CredentialPool::invalidate(const std::string& provider) {
    std::lock_guard<std::mutex> lock(mu_);
    slots_.erase(provider);
}

void CredentialPool::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    slots_.clear();
}

std::size_t CredentialPool::evict_expired(
    std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t evicted = 0;
    for (auto it = slots_.begin(); it != slots_.end();) {
        auto& creds = it->second.creds;
        auto before = creds.size();
        creds.erase(
            std::remove_if(creds.begin(), creds.end(),
                           [&](const PooledCredential& c) {
                               return c.is_expired(now);
                           }),
            creds.end());
        evicted += before - creds.size();
        if (creds.empty()) {
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    return evicted;
}

std::size_t CredentialPool::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t total = 0;
    for (const auto& [_, slot] : slots_) {
        total += slot.creds.size();
    }
    return total;
}

CredentialPool& CredentialPool::global() {
    static CredentialPool instance;
    return instance;
}

}  // namespace hermes::llm
