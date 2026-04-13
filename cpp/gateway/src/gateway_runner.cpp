#include <hermes/gateway/gateway_runner.hpp>

#include <hermes/agent/ai_agent.hpp>
#include <hermes/core/retry.hpp>
#include <hermes/gateway/status.hpp>

#include <fstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace hermes::gateway {

namespace fs = std::filesystem;

GatewayRunner::GatewayRunner(GatewayConfig config,
                             SessionStore* sessions,
                             HookRegistry* hooks)
    : config_(std::move(config)),
      sessions_(sessions),
      hooks_(hooks) {}

GatewayRunner::~GatewayRunner() {
    stop_watchers_.store(true, std::memory_order_release);
    for (auto& t : watcher_threads_) {
        if (t.joinable()) t.join();
    }
}

void GatewayRunner::register_adapter(
    std::unique_ptr<BasePlatformAdapter> adapter) {
    adapters_.push_back(std::move(adapter));
}

void GatewayRunner::start() {
    RuntimeStatus status;
    status.state = "starting";
    status.timestamp = std::chrono::system_clock::now();
    write_runtime_status(status);

    for (auto& adapter : adapters_) {
        auto p = adapter->platform();
        auto it = config_.platforms.find(p);
        if (it != config_.platforms.end() && it->second.enabled) {
            bool ok = adapter->connect();
            status.platform_states[p] = ok ? "connected" : "disconnected";
        }
    }

    status.state = "running";
    status.timestamp = std::chrono::system_clock::now();
    write_runtime_status(status);

    if (hooks_) {
        hooks_->emit(EVT_GATEWAY_STARTUP);
    }
}

void GatewayRunner::stop() {
    stop_watchers_.store(true, std::memory_order_release);

    // Stop all running agents.
    {
        std::lock_guard<std::mutex> lock(running_agents_mu_);
        for (auto& [key, pair] : running_agents_) {
            if (pair.first) {
                pair.first->request_stop();
            }
        }
        running_agents_.clear();
    }

    for (auto& adapter : adapters_) {
        adapter->disconnect();
    }

    // Join watcher threads.
    for (auto& t : watcher_threads_) {
        if (t.joinable()) t.join();
    }
    watcher_threads_.clear();

    RuntimeStatus status;
    status.state = "stopping";
    status.timestamp = std::chrono::system_clock::now();
    write_runtime_status(status);
}

bool GatewayRunner::try_dispatch_command(const std::string& session_key,
                                          const MessageEvent& event) {
    if (event.text.empty() || event.text[0] != '/') return false;

    // Extract the command word.
    auto space = event.text.find(' ');
    auto cmd = event.text.substr(0, space);

    bool agent_running = is_agent_running(session_key);

    if (cmd == "/stop" && agent_running) {
        std::lock_guard<std::mutex> lock(running_agents_mu_);
        auto it = running_agents_.find(session_key);
        if (it != running_agents_.end() && it->second.first) {
            it->second.first->request_stop();
        }
        return true;
    }

    if ((cmd == "/new" || cmd == "/reset") && agent_running) {
        // Stop agent + clear history.
        {
            std::lock_guard<std::mutex> lock(running_agents_mu_);
            auto it = running_agents_.find(session_key);
            if (it != running_agents_.end() && it->second.first) {
                it->second.first->request_stop();
            }
            running_agents_.erase(session_key);
        }
        {
            std::lock_guard<std::mutex> lock(agent_cache_mu_);
            agent_cache_.erase(session_key);
        }
        if (sessions_) {
            sessions_->reset_session(session_key);
        }
        return true;
    }

    if ((cmd == "/approve" || cmd == "/deny") && agent_running) {
        // Route to approval queue — for now, queue as pending.
        handle_busy_session(session_key, event);
        return true;
    }

    return false;
}

void GatewayRunner::handle_message(const MessageEvent& event) {
    if (!sessions_) return;

    // Get or create session for this source.
    auto session_key = sessions_->get_or_create_session(event.source);

    // Try command dispatch first.
    if (try_dispatch_command(session_key, event)) return;

    // If agent is running and this isn't a command, queue the message.
    if (is_agent_running(session_key)) {
        handle_busy_session(session_key, event);
        return;
    }

    // Normal message processing — agent invocation would happen here.
    // For now, record the message in the session transcript.
    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = event.text;
    sessions_->append_message(session_key, msg);
}

void GatewayRunner::send_to_platform(const std::string& platform_name,
                                     const std::string& chat_id,
                                     const std::string& content) {
    Platform target;
    try {
        target = platform_from_string(platform_name);
    } catch (...) {
        throw std::runtime_error("unknown platform: " + platform_name);
    }

    for (auto& adapter : adapters_) {
        if (adapter->platform() == target) {
            if (!adapter->send(chat_id, content)) {
                throw std::runtime_error(
                    "send failed for platform: " + platform_name);
            }
            return;
        }
    }

    throw std::runtime_error(
        "no adapter registered for platform: " + platform_name);
}

std::vector<GatewayRunner::AdapterInfo> GatewayRunner::list_adapters() const {
    std::vector<AdapterInfo> out;
    out.reserve(adapters_.size());
    for (const auto& adapter : adapters_) {
        auto p = adapter->platform();
        auto name = platform_to_string(p);
        bool connected = false;
        auto status = read_runtime_status();
        if (status) {
            auto it = status->platform_states.find(p);
            if (it != status->platform_states.end()) {
                connected = (it->second == "connected");
            }
        }
        out.push_back({std::move(name), connected});
    }
    return out;
}

// --- Agent lifecycle ---

void GatewayRunner::set_agent_factory(AgentFactory factory) {
    agent_factory_ = std::move(factory);
}

std::shared_ptr<hermes::agent::AIAgent>
GatewayRunner::get_or_create_agent(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(agent_cache_mu_);
    auto it = agent_cache_.find(session_key);
    if (it != agent_cache_.end()) {
        return it->second;
    }

    std::shared_ptr<hermes::agent::AIAgent> agent;
    if (agent_factory_) {
        agent = agent_factory_(session_key);
    }
    // If no factory or factory returned null, store null — callers should
    // check. In production, a factory would always be set.
    agent_cache_[session_key] = agent;
    return agent;
}

bool GatewayRunner::is_agent_running(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(running_agents_mu_);
    return running_agents_.count(session_key) > 0;
}

void GatewayRunner::mark_agent_running(
    const std::string& session_key,
    std::shared_ptr<hermes::agent::AIAgent> agent) {
    std::lock_guard<std::mutex> lock(running_agents_mu_);
    running_agents_[session_key] = {std::move(agent),
                                     std::chrono::system_clock::now()};
}

void GatewayRunner::mark_agent_finished(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(running_agents_mu_);
    running_agents_.erase(session_key);
}

void GatewayRunner::evict_stale_agents() {
    std::lock_guard<std::mutex> lock(running_agents_mu_);
    auto now = std::chrono::system_clock::now();
    for (auto it = running_agents_.begin(); it != running_agents_.end();) {
        auto elapsed = now - it->second.second;
        if (elapsed > STALE_AGENT_THRESHOLD) {
            if (it->second.first) {
                it->second.first->request_stop();
            }
            it = running_agents_.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Pending message queue ---

void GatewayRunner::handle_busy_session(const std::string& session_key,
                                         const MessageEvent& event) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    pending_messages_[session_key].push_back(event);
}

std::vector<MessageEvent>
GatewayRunner::drain_pending(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    auto it = pending_messages_.find(session_key);
    if (it == pending_messages_.end()) return {};
    auto msgs = std::move(it->second);
    pending_messages_.erase(it);
    return msgs;
}

// --- Hook discovery ---

void GatewayRunner::discover_hooks(const fs::path& hooks_dir) {
    if (!hooks_ || !fs::exists(hooks_dir)) return;

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(hooks_dir, ec)) {
        if (!entry.is_directory()) continue;

        auto hook_yaml = entry.path() / "HOOK.yaml";
        if (!fs::exists(hook_yaml)) continue;

        // Read HOOK.yaml — parse name and events.
        std::ifstream ifs(hook_yaml);
        if (!ifs.is_open()) continue;

        try {
            auto j = nlohmann::json::parse(ifs);
            std::string name = j.value("name", entry.path().filename().string());
            auto events = j.value("events", std::vector<std::string>{});

            for (const auto& event_pattern : events) {
                hooks_->register_hook(
                    event_pattern,
                    [name, event_pattern](const std::string& evt,
                                          const nlohmann::json& ctx) {
                        // Log the event — actual Python handler execution
                        // is deferred to a later phase.
                        (void)evt;
                        (void)ctx;
                        // In production this would invoke the hook's
                        // handler script.
                    });
            }
        } catch (...) {
            // Malformed HOOK.yaml — skip.
            continue;
        }
    }
}

void GatewayRunner::register_boot_md_hook(const fs::path& boot_md_path) {
    if (!hooks_) return;

    // Capture path by value.
    auto path = boot_md_path;
    hooks_->register_hook(
        EVT_GATEWAY_STARTUP,
        [this, path](const std::string& /*evt*/,
                     const nlohmann::json& /*ctx*/) {
            if (!fs::exists(path)) return;
            std::ifstream ifs(path);
            if (!ifs.is_open()) return;

            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            if (content.empty()) return;

            // Inject as first user message to all active sessions.
            // This is a startup-only operation.
            if (sessions_) {
                nlohmann::json msg;
                msg["role"] = "user";
                msg["content"] = content;
                msg["_boot_md"] = true;
                // Store in a well-known session key for the boot message.
                sessions_->append_message("__boot__",  msg);
            }
        });
}

// --- Background watchers ---

void GatewayRunner::start_session_expiry_watcher() {
    watcher_threads_.emplace_back([this] {
        while (!stop_watchers_.load(std::memory_order_acquire)) {
            // Check every 5 minutes.
            for (int i = 0; i < 300 && !stop_watchers_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (stop_watchers_.load()) break;

            // Flush expired session memories — delegate to SessionStore.
            // In production, SessionStore would expose an expire() method.
        }
    });
}

void GatewayRunner::start_reconnect_watcher() {
    watcher_threads_.emplace_back([this] {
        // Per-platform attempt counter — drives jittered exponential backoff.
        std::unordered_map<int, int> attempts;
        // Per-platform fatal flag — adapters classified Fatal are not retried.
        std::unordered_set<int> fatal;

        while (!stop_watchers_.load(std::memory_order_acquire)) {
            // Sleep 1s polling-tick — actual backoff is enforced per
            // adapter via the attempt counter.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stop_watchers_.load()) break;

            auto status = read_runtime_status();
            if (!status) continue;

            for (auto& adapter : adapters_) {
                auto p = adapter->platform();
                auto pkey = static_cast<int>(p);
                if (fatal.count(pkey)) continue;

                auto cit = config_.platforms.find(p);
                if (cit == config_.platforms.end() || !cit->second.enabled)
                    continue;

                auto sit = status->platform_states.find(p);
                if (sit == status->platform_states.end() ||
                    sit->second != "disconnected") {
                    attempts.erase(pkey);
                    continue;
                }

                int n = ++attempts[pkey];
                auto delay = hermes::core::retry::jittered_backoff(
                    n, std::chrono::seconds(1), std::chrono::seconds(300));
                // Wait the per-adapter back-off before retrying.
                auto until = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < until &&
                       !stop_watchers_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (stop_watchers_.load()) break;

                bool ok = false;
                try {
                    ok = adapter->connect();
                } catch (...) {
                    ok = false;
                }
                if (ok) {
                    attempts.erase(pkey);
                    auto s = read_runtime_status().value_or(RuntimeStatus{});
                    s.platform_states[p] = "connected";
                    s.timestamp = std::chrono::system_clock::now();
                    write_runtime_status(s);
                } else if (adapter->last_error_kind() ==
                           AdapterErrorKind::Fatal) {
                    fatal.insert(pkey);
                    auto s = read_runtime_status().value_or(RuntimeStatus{});
                    s.platform_states[p] = "fatal";
                    s.timestamp = std::chrono::system_clock::now();
                    write_runtime_status(s);
                }
            }
        }
    });
}

void GatewayRunner::start_process_watcher() {
    watcher_threads_.emplace_back([this] {
        while (!stop_watchers_.load(std::memory_order_acquire)) {
            // Poll every 10 seconds.
            for (int i = 0; i < 10 && !stop_watchers_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (stop_watchers_.load()) break;

            // Poll ProcessRegistry for completed processes and deliver
            // notifications.  ProcessRegistry integration is deferred —
            // this watcher is the skeleton.
        }
    });
}

}  // namespace hermes::gateway
