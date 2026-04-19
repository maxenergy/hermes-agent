#include <hermes/gateway/session_manager.hpp>

#include <algorithm>
#include <sstream>

namespace hermes::gateway {

// --- Sentinel ------------------------------------------------------------

std::shared_ptr<void> AgentPendingSentinel::instance() {
    // A shared dummy object.  ``shared_ptr<int>`` works because we only
    // compare pointer identity in is_agent_pending / mark_pending.
    static std::shared_ptr<void> s = std::make_shared<int>(0);
    return s;
}

// --- AgentConfigSignature ------------------------------------------------

bool AgentConfigSignature::operator==(const AgentConfigSignature& o) const {
    return model == o.model && provider == o.provider &&
           base_url == o.base_url && api_mode == o.api_mode &&
           personality == o.personality &&
           reasoning_enabled == o.reasoning_enabled &&
           reasoning_effort == o.reasoning_effort &&
           service_tier == o.service_tier && toolset == o.toolset;
}

std::string AgentConfigSignature::serialize() const {
    std::ostringstream os;
    os << model << '|' << provider << '|' << base_url << '|' << api_mode
       << '|' << personality << '|' << (reasoning_enabled ? 1 : 0) << '|'
       << reasoning_effort << '|' << service_tier << "|[";
    for (size_t i = 0; i < toolset.size(); ++i) {
        if (i) os << ',';
        os << toolset[i];
    }
    os << ']';
    return os.str();
}

// --- SessionManager ------------------------------------------------------

SessionManager::SessionManager() = default;
SessionManager::~SessionManager() = default;

std::string SessionManager::session_key_for_source(
    const SessionSource& source, bool group_per_user,
    bool thread_per_user) const {
    // The canonical key shape is ``<platform>:<chat_id>[:<thread>][:<user>]``.
    std::ostringstream os;
    os << platform_to_string(source.platform) << ':' << source.chat_id;

    if (thread_per_user && !source.thread_id.empty()) {
        os << ':' << source.thread_id;
    }
    bool is_group = source.chat_type == "group" || source.chat_type == "channel";
    if (is_group && group_per_user && !source.user_id.empty()) {
        os << ':' << source.user_id;
    }
    return os.str();
}

bool SessionManager::mark_pending(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (running_.count(session_key)) return false;
    auto now = std::chrono::system_clock::now();
    running_[session_key] = {AgentPendingSentinel::instance(), now, now, true};
    return true;
}

void SessionManager::mark_running(
    const std::string& session_key,
    std::shared_ptr<hermes::agent::AIAgent> agent) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    auto now = std::chrono::system_clock::now();
    if (it == running_.end()) {
        running_[session_key] = {std::static_pointer_cast<void>(agent), now,
                                  now, false};
        return;
    }
    it->second.agent_or_sentinel = std::static_pointer_cast<void>(agent);
    it->second.is_pending = false;
    it->second.last_activity = now;
}

bool SessionManager::is_agent_running(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_.count(session_key) > 0;
}

bool SessionManager::is_agent_pending(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    return it != running_.end() && it->second.is_pending;
}

void SessionManager::mark_finished(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    running_.erase(session_key);
}

void SessionManager::touch_activity(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    if (it != running_.end()) {
        it->second.last_activity = std::chrono::system_clock::now();
    }
}

std::shared_ptr<hermes::agent::AIAgent> SessionManager::running_agent(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    if (it == running_.end() || it->second.is_pending) return nullptr;
    return std::static_pointer_cast<hermes::agent::AIAgent>(
        it->second.agent_or_sentinel);
}

std::size_t SessionManager::running_agent_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_.size();
}

std::vector<RunningAgentInfo> SessionManager::snapshot_running() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<RunningAgentInfo> out;
    out.reserve(running_.size());
    for (const auto& [key, entry] : running_) {
        out.push_back({key, entry.started_at, entry.last_activity,
                        entry.is_pending});
    }
    return out;
}

std::vector<std::string> SessionManager::evict_idle(
    std::chrono::seconds idle_threshold) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> evicted;
    auto now = std::chrono::system_clock::now();
    for (auto it = running_.begin(); it != running_.end();) {
        auto idle = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.last_activity);
        if (idle > idle_threshold) {
            evicted.push_back(it->first);
            it = running_.erase(it);
        } else {
            ++it;
        }
    }
    return evicted;
}

void SessionManager::set_agent_factory(AgentFactory factory) {
    std::lock_guard<std::mutex> lock(mu_);
    factory_ = std::move(factory);
}

void SessionManager::set_agent_release(AgentReleaseFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    release_fn_ = std::move(fn);
}

void SessionManager::set_agent_cache_max_size(std::size_t max_size) {
    std::lock_guard<std::mutex> lock(mu_);
    cache_max_size_ = max_size;
}

std::size_t SessionManager::agent_cache_max_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_max_size_;
}

void SessionManager::set_agent_cache_idle_ttl(std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    cache_idle_ttl_ = ttl;
}

std::chrono::seconds SessionManager::agent_cache_idle_ttl() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_idle_ttl_;
}

std::size_t SessionManager::agent_cache_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.size();
}

std::vector<std::shared_ptr<hermes::agent::AIAgent>>
SessionManager::enforce_cache_cap_locked_() {
    std::vector<std::shared_ptr<hermes::agent::AIAgent>> evicted;
    if (cache_.size() <= cache_max_size_) return evicted;

    // Build identity set of mid-turn agents so we don't tear down their
    // clients while an active turn is using them.  Matches upstream
    // _enforce_agent_cache_cap() "running_ids" snapshot.
    std::unordered_map<hermes::agent::AIAgent*, bool> mid_turn;
    for (auto& [_, entry] : running_) {
        if (entry.is_pending) continue;
        auto* raw = static_cast<hermes::agent::AIAgent*>(
            entry.agent_or_sentinel.get());
        if (raw) mid_turn[raw] = true;
    }

    // Walk LRU -> MRU, evicting excess entries that aren't mid-turn.  If
    // a mid-turn agent is in the excess window we SKIP without
    // compensating — freshly-inserted sessions must not be punished to
    // protect a long-lived busy one.  (Matches the commit comment.)
    std::size_t excess = cache_.size() - cache_max_size_;
    auto it = cache_order_.begin();
    while (excess > 0 && it != cache_order_.end()) {
        auto entry_it = cache_.find(*it);
        if (entry_it == cache_.end()) {
            it = cache_order_.erase(it);
            continue;
        }
        auto* raw = entry_it->second.agent.get();
        if (raw && mid_turn.count(raw)) {
            ++it;
            --excess;  // count against the excess window; don't substitute
            continue;
        }
        evicted.push_back(entry_it->second.agent);
        cache_.erase(entry_it);
        it = cache_order_.erase(it);
        --excess;
    }
    return evicted;
}

std::shared_ptr<hermes::agent::AIAgent> SessionManager::get_or_create_agent(
    const std::string& session_key) {
    std::unique_lock<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();
    auto it = cache_.find(session_key);
    if (it != cache_.end()) {
        // LRU refresh: move to MRU tail.
        cache_order_.splice(cache_order_.end(), cache_order_,
                             it->second.order_it);
        it->second.last_activity = now;
        return it->second.agent;
    }
    if (!factory_) return nullptr;
    auto factory = factory_;
    lock.unlock();
    auto agent = factory(session_key);
    lock.lock();
    // Re-check in case another thread raced us.
    auto re_it = cache_.find(session_key);
    if (re_it != cache_.end()) {
        cache_order_.splice(cache_order_.end(), cache_order_,
                             re_it->second.order_it);
        re_it->second.last_activity = now;
        return re_it->second.agent;
    }
    cache_order_.push_back(session_key);
    auto order_it = std::prev(cache_order_.end());
    cache_[session_key] = {agent, order_it, now};

    // Enforce the cap after the insert.  Pop out the evicted list so we
    // can invoke release_fn_ without holding mu_.
    auto evicted = enforce_cache_cap_locked_();
    auto release_fn = release_fn_;
    lock.unlock();

    if (release_fn) {
        for (auto& a : evicted) {
            try { release_fn(a); } catch (...) {}
        }
    }
    return agent;
}

std::shared_ptr<hermes::agent::AIAgent> SessionManager::evict_cached_agent(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(session_key);
    if (it == cache_.end()) return nullptr;
    auto old = it->second.agent;
    cache_order_.erase(it->second.order_it);
    cache_.erase(it);
    return old;
}

std::size_t SessionManager::sweep_idle_cached_agents() {
    std::vector<std::shared_ptr<hermes::agent::AIAgent>> evicted;
    AgentReleaseFn release_fn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        release_fn = release_fn_;

        std::unordered_map<hermes::agent::AIAgent*, bool> mid_turn;
        for (auto& [_, entry] : running_) {
            if (entry.is_pending) continue;
            auto* raw = static_cast<hermes::agent::AIAgent*>(
                entry.agent_or_sentinel.get());
            if (raw) mid_turn[raw] = true;
        }

        auto ttl_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                cache_idle_ttl_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            auto* raw = it->second.agent.get();
            if (raw && mid_turn.count(raw)) {
                ++it;
                continue;
            }
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.last_activity);
            if (idle > ttl_ms) {
                evicted.push_back(it->second.agent);
                cache_order_.erase(it->second.order_it);
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (release_fn) {
        for (auto& a : evicted) {
            try { release_fn(a); } catch (...) {}
        }
    }
    return evicted.size();
}

// --- Model override ------------------------------------------------------

void SessionManager::set_session_model_override(
    const std::string& session_key, const std::string& model) {
    std::lock_guard<std::mutex> lock(mu_);
    if (model.empty()) {
        model_overrides_.erase(session_key);
    } else {
        model_overrides_[session_key] = model;
    }
}

std::optional<std::string> SessionManager::get_session_model_override(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = model_overrides_.find(session_key);
    if (it == model_overrides_.end()) return std::nullopt;
    return it->second;
}

void SessionManager::clear_session_model_override(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    model_overrides_.erase(session_key);
}

bool SessionManager::is_intentional_model_switch(
    const std::string& session_key, const std::string& agent_model) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = model_overrides_.find(session_key);
    return it != model_overrides_.end() && it->second == agent_model;
}

void SessionManager::apply_session_model_override(
    const std::string& session_key, nlohmann::json& kwargs) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = model_overrides_.find(session_key);
    if (it == model_overrides_.end()) return;
    if (!kwargs.is_object()) kwargs = nlohmann::json::object();
    kwargs["model"] = it->second;
}

// --- Signature cache -----------------------------------------------------

AgentConfigSignature SessionManager::current_signature(
    const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = signatures_.find(session_key);
    if (it == signatures_.end()) return {};
    return it->second;
}

void SessionManager::set_signature(const std::string& session_key,
                                     AgentConfigSignature sig) {
    std::lock_guard<std::mutex> lock(mu_);
    signatures_[session_key] = std::move(sig);
}

// --- Race-condition helpers (upstream 3a635145) -------------------------

bool SessionManager::clear_interrupt_flag(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    if (it == running_.end()) return false;
    it->second.interrupt = false;
    // Refresh activity so the stale-eviction sweep doesn't snap at us
    // right after a turn-chain transition.
    it->second.last_activity = std::chrono::system_clock::now();
    return true;
}

bool SessionManager::is_interrupt_set(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    if (it == running_.end()) return false;
    return it->second.interrupt;
}

void SessionManager::set_interrupt_flag(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(session_key);
    if (it == running_.end()) return;
    it->second.interrupt = true;
}

bool SessionManager::finalize_with_late_drain(
    const std::string& session_key, PendingQueue* queue,
    LateDrainStarter drain_starter) {
    // Step 1: see if a late-arrival message landed in the queue during
    // the cleanup awaits (Python: typing_task cancel, stop_typing).
    // The busy-handler for any concurrent inbound event still saw the
    // _active_sessions entry live and enqueued.  We must drain that
    // message instead of dropping it.
    std::optional<MessageEvent> late;
    if (queue) late = queue->dequeue(session_key);

    if (late && drain_starter) {
        // Late-arrival found: keep the session entry populated (the
        // drain task's own lifecycle will mark_finished when it ends),
        // and clear the interrupt flag so the drain agent starts clean.
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = running_.find(session_key);
            if (it != running_.end()) {
                it->second.interrupt = false;
                it->second.last_activity =
                    std::chrono::system_clock::now();
            }
        }
        try {
            drain_starter(session_key, std::move(*late));
        } catch (...) {
            // drain_starter must not throw, but tolerate it — we still
            // need to release the session if the dispatch failed.
            mark_finished(session_key);
            return false;
        }
        return true;
    }

    // No late arrival — normal finalize.  If the caller re-enqueued
    // during our late-check, the drain_starter path will have taken
    // it; otherwise we release now.
    mark_finished(session_key);
    return false;
}

}  // namespace hermes::gateway
