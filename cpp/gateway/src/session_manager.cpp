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

std::shared_ptr<hermes::agent::AIAgent> SessionManager::get_or_create_agent(
    const std::string& session_key) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = cache_.find(session_key);
    if (it != cache_.end()) return it->second;
    if (!factory_) return nullptr;
    auto factory = factory_;
    lock.unlock();
    auto agent = factory(session_key);
    lock.lock();
    cache_[session_key] = agent;
    return agent;
}

std::shared_ptr<hermes::agent::AIAgent> SessionManager::evict_cached_agent(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(session_key);
    if (it == cache_.end()) return nullptr;
    auto old = it->second;
    cache_.erase(it);
    return old;
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

}  // namespace hermes::gateway
