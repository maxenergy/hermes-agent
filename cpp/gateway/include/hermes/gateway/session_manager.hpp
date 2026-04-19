// Per-session agent lifecycle tracking for the gateway.
//
// Mirrors the Python ``GatewayRunner._running_agents`` + agent cache + model
// override + config-signature machinery in gateway/run.py.  Kept in its own
// module because the runner itself wants to remain a thin orchestrator.
#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <hermes/gateway/message_pipeline.hpp>  // PendingQueue + MessageEvent
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

    // Agent-cache tuning.  Defaults mirror gateway/run.py:
    //   _AGENT_CACHE_MAX_SIZE       = 128
    //   _AGENT_CACHE_IDLE_TTL_SECS  = 3600
    // (upstream 8d7b7feb).  Exposed so tests can override without
    // churning class state.
    static constexpr std::size_t kDefaultAgentCacheMax = 128;
    static constexpr std::chrono::seconds kDefaultAgentIdleTtl{3600};

    SessionManager();
    ~SessionManager();

    // --- LRU + idle-TTL knobs -------------------------------------------
    void set_agent_cache_max_size(std::size_t max_size);
    std::size_t agent_cache_max_size() const;
    void set_agent_cache_idle_ttl(std::chrono::seconds ttl);
    std::chrono::seconds agent_cache_idle_ttl() const;
    std::size_t agent_cache_size() const;

    // Called from the session-expiry watcher (or tests).  Evicts cached
    // agents whose last_activity is older than ``idle_ttl``.  Agents in
    // ``running_`` are skipped — tearing down an active turn's clients
    // mid-flight would crash the request.  Returns the number evicted.
    std::size_t sweep_idle_cached_agents();

    // Release callback invoked on eviction so the caller can run the
    // Python ``_release_evicted_agent_soft`` equivalent (close the LLM
    // client only; leave process_registry / terminal sandbox intact
    // for a resuming agent to inherit).
    using AgentReleaseFn =
        std::function<void(std::shared_ptr<hermes::agent::AIAgent>)>;
    void set_agent_release(AgentReleaseFn fn);

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

    // --- Race-condition helpers (upstream 3a635145) ---------------------
    //
    // Two races in the Python ``_process_message_background`` turn chain
    // are closed by keeping the ``_active_sessions`` entry live across
    // the drain cycles and deferring its deletion until after we've
    // checked for late-arrival pending messages.

    // Python: ``_active.clear()`` without deleting the entry.  Resets
    // the interrupt signal for the next turn in the chain but leaves
    // the session marked as running so concurrent inbound messages take
    // the busy-handler path (queue + interrupt) instead of spawning a
    // second agent.  Returns false if the session is not known.
    bool clear_interrupt_flag(const std::string& session_key);

    // True if the session has been interrupted (request_stop / agent
    // cancel).  Adapters check this between drain cycles.
    bool is_interrupt_set(const std::string& session_key) const;

    // Set the interrupt flag — called by /stop and by the busy-handler
    // path when a new message arrives for a running session.
    void set_interrupt_flag(const std::string& session_key);

    // Upstream R6 finally-path fix: call at the end of the background
    // turn to release the session.  If ``queue`` still has a message
    // for the session_key (late arrival during typing_task.cancel()
    // await), ``drain_starter`` is invoked with the popped event and
    // the session entry is LEFT in place — the drain task's own
    // lifecycle cleans it up.  Otherwise the entry is removed normally.
    // Returns true when a late drain was dispatched.
    using LateDrainStarter =
        std::function<void(const std::string&, MessageEvent)>;
    bool finalize_with_late_drain(const std::string& session_key,
                                    PendingQueue* queue,
                                    LateDrainStarter drain_starter);

private:
    struct RunningEntry {
        std::shared_ptr<void> agent_or_sentinel;
        std::chrono::system_clock::time_point started_at;
        std::chrono::system_clock::time_point last_activity;
        bool is_pending = false;
        // Mirrors asyncio.Event — set by /stop and the busy-handler
        // race; cleared by clear_interrupt_flag.  See upstream 3a635145.
        bool interrupt = false;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, RunningEntry> running_;

    // LRU agent cache (upstream 8d7b7feb).  ``cache_order_`` is the
    // LRU list (front = LRU, back = MRU); ``cache_`` maps key to the
    // cached agent + its iterator into ``cache_order_`` for O(1)
    // refresh / eviction.  ``cache_last_activity_`` tracks the per-
    // agent last activity for idle-TTL eviction.
    struct CacheEntry {
        std::shared_ptr<hermes::agent::AIAgent> agent;
        std::list<std::string>::iterator order_it;
        std::chrono::steady_clock::time_point last_activity;
    };
    std::list<std::string> cache_order_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::size_t cache_max_size_ = kDefaultAgentCacheMax;
    std::chrono::seconds cache_idle_ttl_ = kDefaultAgentIdleTtl;
    AgentReleaseFn release_fn_;

    std::unordered_map<std::string, std::string> model_overrides_;
    std::unordered_map<std::string, AgentConfigSignature> signatures_;
    AgentFactory factory_;

    // Caller must hold mu_.  Enforces cache_max_size_ by evicting the
    // LRU-most entries that are NOT currently mid-turn.  Mid-turn
    // agents are skipped without compensating (matches upstream
    // 8d7b7feb "never evict mid-turn agents" behaviour).  Returns the
    // evicted agents so the caller can invoke release_fn_ outside the
    // lock.
    std::vector<std::shared_ptr<hermes::agent::AIAgent>>
    enforce_cache_cap_locked_();
};

}  // namespace hermes::gateway
