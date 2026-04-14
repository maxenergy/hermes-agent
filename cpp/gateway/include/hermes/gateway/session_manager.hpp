// Per-session agent lifecycle tracking for the gateway.
//
// Mirrors the Python ``GatewayRunner._running_agents`` + agent cache + model
// override + config-signature machinery in gateway/run.py.  Kept in its own
// module because the runner itself wants to remain a thin orchestrator.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <hermes/gateway/session_store.hpp>

namespace hermes::agent {
class AIAgent;
}

namespace hermes::gateway {

// Sentinel shared_ptr stored in the ``running`` map to indicate a
// session has a pending agent turn queued but not yet started.
class AgentPendingSentinel {
public:
    static std::shared_ptr<void> instance();
};

// Snapshot of the agent configuration keyed to a session — any change
// forces a re-provisioning.  Mirrors ``_agent_config_signature`` tuple.
struct AgentConfigSignature {
    std::string model;
    std::string provider;
    std::string base_url;
    std::string api_mode;
    std::string personality;
    bool reasoning_enabled = false;
    std::string reasoning_effort;
    std::string service_tier;
    std::vector<std::string> toolset;

    bool operator==(const AgentConfigSignature& o) const;
    bool operator!=(const AgentConfigSignature& o) const {
        return !(*this == o);
    }
    std::string serialize() const;
};

// Snapshot row returned from ``snapshot_running``.
struct RunningAgentInfo {
    std::string session_key;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point last_activity;
    bool is_pending = false;
};

class SessionManager {
public:
    using AgentFactory =
        std::function<std::shared_ptr<hermes::agent::AIAgent>(const std::string&)>;

    SessionManager();
    ~SessionManager();

    // --- Session key derivation ------------------------------------------

    // Build a canonical session key from a SessionSource, honoring the
    // group_per_user / thread_per_user routing flags.
    std::string session_key_for_source(const SessionSource& source,
                                        bool group_per_user,
                                        bool thread_per_user) const;

    // --- Pending + running bookkeeping -----------------------------------

    // Mark a session as pending (a turn has been queued but the agent has
    // not yet been provisioned).  Returns false if the session is already
    // pending or running.
    bool mark_pending(const std::string& session_key);

    // Promote a pending session to running with a live agent.  Idempotent.
    void mark_running(const std::string& session_key,
                       std::shared_ptr<hermes::agent::AIAgent> agent);

    bool is_agent_running(const std::string& session_key) const;
    bool is_agent_pending(const std::string& session_key) const;

    // Clear the running / pending record for a session.
    void mark_finished(const std::string& session_key);

    // Bump ``last_activity`` for an existing running session.
    void touch_activity(const std::string& session_key);

    // Return the live agent (null for pending / unknown sessions).
    std::shared_ptr<hermes::agent::AIAgent> running_agent(
        const std::string& session_key) const;

    std::size_t running_agent_count() const;
    std::vector<RunningAgentInfo> snapshot_running() const;

    // Remove any running/pending row whose ``last_activity`` is older
    // than ``idle_threshold``.  Returns the evicted session keys.
    std::vector<std::string> evict_idle(std::chrono::seconds idle_threshold);

    // --- Agent factory + cache -------------------------------------------

    void set_agent_factory(AgentFactory factory);

    // Return a cached agent for ``session_key`` or invoke the factory.
    // Returns null if no factory is registered.
    std::shared_ptr<hermes::agent::AIAgent> get_or_create_agent(
        const std::string& session_key);

    // Drop the cached agent for ``session_key`` and return it (for the
    // caller to shut down or stash as ``old_agent_for_refresh``).
    std::shared_ptr<hermes::agent::AIAgent> evict_cached_agent(
        const std::string& session_key);

    // --- Per-session model override --------------------------------------

    void set_session_model_override(const std::string& session_key,
                                      const std::string& model);
    std::optional<std::string> get_session_model_override(
        const std::string& session_key) const;
    void clear_session_model_override(const std::string& session_key);

    // Mirrors gateway/run.py::_is_intentional_model_switch.
    bool is_intentional_model_switch(const std::string& session_key,
                                       const std::string& agent_model) const;

    // Apply a recorded model override onto an agent-invocation kwargs
    // blob.  Mirrors gateway/run.py::_apply_session_model_override.
    void apply_session_model_override(const std::string& session_key,
                                        nlohmann::json& kwargs) const;

    // --- Config signature cache ------------------------------------------

    AgentConfigSignature current_signature(const std::string& session_key) const;
    void set_signature(const std::string& session_key,
                        AgentConfigSignature sig);

private:
    struct RunningEntry {
        std::shared_ptr<void> agent_or_sentinel;
        std::chrono::system_clock::time_point started_at;
        std::chrono::system_clock::time_point last_activity;
        bool is_pending = false;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, RunningEntry> running_;
    std::unordered_map<std::string,
                        std::shared_ptr<hermes::agent::AIAgent>>
        cache_;
    std::unordered_map<std::string, std::string> model_overrides_;
    std::unordered_map<std::string, AgentConfigSignature> signatures_;
    AgentFactory factory_;
};

}  // namespace hermes::gateway
