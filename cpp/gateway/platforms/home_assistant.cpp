// Phase 12 — Home Assistant platform adapter implementation.
#include "home_assistant.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

HomeAssistantAdapter::HomeAssistantAdapter(Config cfg)
    : cfg_(std::move(cfg)) {}

HomeAssistantAdapter::HomeAssistantAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* HomeAssistantAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool HomeAssistantAdapter::connect() {
    if (cfg_.hass_token.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // Verify connectivity via HA REST API.
    try {
        auto resp = transport->get(
            cfg_.hass_url + "/api/",
            {{"Authorization", "Bearer " + cfg_.hass_token}});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void HomeAssistantAdapter::disconnect() {}

bool HomeAssistantAdapter::send(const std::string& chat_id,
                                const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // Fire an event via HA REST API. chat_id is used as the event_type
    // or notification service target.
    nlohmann::json payload = {
        {"message", content},
        {"title", "Hermes Agent"},
        {"data", {{"chat_id", chat_id}}}
    };

    try {
        // Use the persistent notification service.
        auto resp = transport->post_json(
            cfg_.hass_url + "/api/services/notify/persistent_notification",
            {{"Authorization", "Bearer " + cfg_.hass_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void HomeAssistantAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
