// Phase 12 — Home Assistant platform adapter implementation.
#include "home_assistant.hpp"

namespace hermes::gateway::platforms {

HomeAssistantAdapter::HomeAssistantAdapter(Config cfg)
    : cfg_(std::move(cfg)) {}

bool HomeAssistantAdapter::connect() {
    if (cfg_.hass_token.empty()) return false;
    // TODO(phase-14+): WebSocket /api/websocket subscription.
    return true;
}

void HomeAssistantAdapter::disconnect() {}

bool HomeAssistantAdapter::send(const std::string& /*chat_id*/,
                                const std::string& /*content*/) {
    // Home Assistant: fire events, not user messages.
    return true;
}

void HomeAssistantAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
