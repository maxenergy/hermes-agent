// Phase 12 — BlueBubbles (iMessage) platform adapter implementation.
#include "bluebubbles.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

BlueBubblesAdapter::BlueBubblesAdapter(Config cfg) : cfg_(std::move(cfg)) {}

BlueBubblesAdapter::BlueBubblesAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* BlueBubblesAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool BlueBubblesAdapter::connect() {
    if (cfg_.server_url.empty() || cfg_.password.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // Health check via server info endpoint.
    try {
        auto resp = transport->get(
            cfg_.server_url + "/api/v1/server/info?password=" + cfg_.password,
            {});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void BlueBubblesAdapter::disconnect() {}

bool BlueBubblesAdapter::send(const std::string& chat_id,
                              const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"chatGuid", chat_id},
        {"message", content},
        {"method", "private-api"}
    };

    try {
        auto resp = transport->post_json(
            cfg_.server_url + "/api/v1/message/text?password=" + cfg_.password,
            {{"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void BlueBubblesAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
