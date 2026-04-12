#include <hermes/gateway/gateway_runner.hpp>

#include <hermes/gateway/status.hpp>

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

}  // namespace hermes::gateway
