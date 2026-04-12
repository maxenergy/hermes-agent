// Phase 12 — BlueBubbles (iMessage) platform adapter implementation.
#include "bluebubbles.hpp"

namespace hermes::gateway::platforms {

BlueBubblesAdapter::BlueBubblesAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool BlueBubblesAdapter::connect() {
    if (cfg_.server_url.empty() || cfg_.password.empty()) return false;
    // TODO(phase-14+): HTTP API health check.
    return true;
}

void BlueBubblesAdapter::disconnect() {}

bool BlueBubblesAdapter::send(const std::string& /*chat_id*/,
                              const std::string& /*content*/) {
    return true;
}

void BlueBubblesAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
