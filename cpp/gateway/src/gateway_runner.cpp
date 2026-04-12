#include <hermes/gateway/gateway_runner.hpp>

#include <hermes/gateway/status.hpp>

#include <stdexcept>

namespace hermes::gateway {

GatewayRunner::GatewayRunner(GatewayConfig config,
                             SessionStore* sessions,
                             HookRegistry* hooks)
    : config_(std::move(config)),
      sessions_(sessions),
      hooks_(hooks) {}

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
    for (auto& adapter : adapters_) {
        adapter->disconnect();
    }

    RuntimeStatus status;
    status.state = "stopping";
    status.timestamp = std::chrono::system_clock::now();
    write_runtime_status(status);
}

void GatewayRunner::handle_message(const MessageEvent& event) {
    if (!sessions_) return;

    // Get or create session for this source.
    sessions_->get_or_create_session(event.source);

    // Phase 12+ will add: command dispatch, agent invocation, etc.
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
        // Check if adapter was connected based on runtime status.
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

}  // namespace hermes::gateway
