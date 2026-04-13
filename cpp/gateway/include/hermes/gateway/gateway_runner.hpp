// Main gateway orchestrator with agent lifecycle management.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <hermes/gateway/gateway_config.hpp>
#include <hermes/gateway/hooks.hpp>
#include <hermes/gateway/session_store.hpp>

namespace hermes::agent {
class AIAgent;
}

namespace hermes::gateway {

// Classification of the most recent connect() failure.  Returned by
// BasePlatformAdapter::last_error_kind() so the gateway runner can
// distinguish "give up — bad credentials" from "retry later — temporary
// outage".  Adapters that don't override return Unknown, which the
// runner treats as retryable.
enum class AdapterErrorKind {
    None,           // last call succeeded
    Retryable,      // 429 / 5xx / network blip — retry with backoff
    Fatal,          // 401 / 403 / lock conflict — stop attempting
    Unknown,        // adapter did not classify; runner treats as retryable
};

class BasePlatformAdapter {
public:
    virtual ~BasePlatformAdapter() = default;
    virtual Platform platform() const = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool send(const std::string& chat_id,
                      const std::string& content) = 0;
    virtual void send_typing(const std::string& /*chat_id*/) {}

    // Override to expose a classification of the most recent connect()
    // failure.  Default is Unknown — the runner will retry indefinitely.
    virtual AdapterErrorKind last_error_kind() const {
        return AdapterErrorKind::Unknown;
    }
};

struct MessageEvent {
    std::string text;
    std::string message_type;  // TEXT|PHOTO|VIDEO|AUDIO|VOICE|DOCUMENT|STICKER
    SessionSource source;
    std::vector<std::string> media_urls;
    std::optional<std::string> reply_to_message_id;
};

class GatewayRunner {
public:
    GatewayRunner(GatewayConfig config, SessionStore* sessions,
                  HookRegistry* hooks);
    ~GatewayRunner();

    void register_adapter(std::unique_ptr<BasePlatformAdapter> adapter);
    void start();  // connect all enabled adapters, emit gateway:startup
    void stop();   // disconnect all, write status

    void handle_message(const MessageEvent& event);

    // Send a message to a specific platform adapter by name.
    void send_to_platform(const std::string& platform_name,
                          const std::string& chat_id,
                          const std::string& content);

    struct AdapterInfo {
        std::string name;
        bool connected;
    };
    std::vector<AdapterInfo> list_adapters() const;

    // --- Agent lifecycle (per-session caching) ---

    // Get or create a cached agent for the given session key.
    // Reuses existing agent to preserve prompt cache.
    std::shared_ptr<hermes::agent::AIAgent> get_or_create_agent(
        const std::string& session_key);

    // Check whether a session currently has a running agent.
    bool is_agent_running(const std::string& session_key) const;

    // Mark agent as running/finished for a session.
    void mark_agent_running(const std::string& session_key,
                            std::shared_ptr<hermes::agent::AIAgent> agent);
    void mark_agent_finished(const std::string& session_key);

    // Evict agents that have been running longer than the stale threshold.
    void evict_stale_agents();
    static constexpr auto STALE_AGENT_THRESHOLD = std::chrono::minutes(30);

    // --- Pending message queue ---

    // Queue a message when the session's agent is busy.
    void handle_busy_session(const std::string& session_key,
                             const MessageEvent& event);

    // Drain pending messages for a session. Returns them in order.
    std::vector<MessageEvent> drain_pending(const std::string& session_key);

    // --- Hook discovery ---

    // Scan a directory for hook subdirectories containing HOOK.yaml.
    void discover_hooks(const std::filesystem::path& hooks_dir);

    // Built-in boot_md hook: inject BOOT.md content on gateway:startup.
    void register_boot_md_hook(const std::filesystem::path& boot_md_path);

    // --- Background watchers ---

    void start_session_expiry_watcher();  // every 5 min, flush expired
    void start_reconnect_watcher();       // exponential backoff reconnect
    void start_process_watcher();         // poll ProcessRegistry

    // Agent factory — allows injection of a custom factory for testing.
    using AgentFactory =
        std::function<std::shared_ptr<hermes::agent::AIAgent>(
            const std::string& session_key)>;
    void set_agent_factory(AgentFactory factory);

private:
    GatewayConfig config_;
    SessionStore* sessions_;
    HookRegistry* hooks_;
    std::vector<std::unique_ptr<BasePlatformAdapter>> adapters_;

    // Per-session agent cache.
    std::unordered_map<std::string, std::shared_ptr<hermes::agent::AIAgent>>
        agent_cache_;
    mutable std::mutex agent_cache_mu_;

    // Running agents with start time.
    std::unordered_map<
        std::string,
        std::pair<std::shared_ptr<hermes::agent::AIAgent>,
                  std::chrono::system_clock::time_point>>
        running_agents_;
    mutable std::mutex running_agents_mu_;

    // Pending message queue per session.
    std::unordered_map<std::string, std::vector<MessageEvent>>
        pending_messages_;
    std::mutex pending_mu_;

    // Agent factory (injectable for testing).
    AgentFactory agent_factory_;

    // Watcher threads.
    std::vector<std::thread> watcher_threads_;
    std::atomic<bool> stop_watchers_{false};

    // Command dispatch helper for handle_message.
    bool try_dispatch_command(const std::string& session_key,
                              const MessageEvent& event);
};

}  // namespace hermes::gateway
