// Phase 12 — Signal platform adapter implementation.
#include "signal.hpp"

namespace hermes::gateway::platforms {

SignalAdapter::SignalAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool SignalAdapter::connect() {
    if (cfg_.http_url.empty() && cfg_.account.empty()) return false;
    // TODO(phase-14+): connect to signal-cli REST API.
    return true;
}

void SignalAdapter::disconnect() {}

bool SignalAdapter::send(const std::string& /*chat_id*/,
                         const std::string& /*content*/) {
    return true;
}

void SignalAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string SignalAdapter::normalize_identifier(const std::string& id) {
    // Stub: pass through; in real implementation would resolve UUID<->phone.
    return id;
}

}  // namespace hermes::gateway::platforms
