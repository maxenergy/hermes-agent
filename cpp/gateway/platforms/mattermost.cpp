// Phase 12 — Mattermost platform adapter implementation.
#include "mattermost.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

MattermostAdapter::MattermostAdapter(Config cfg) : cfg_(std::move(cfg)) {}

MattermostAdapter::MattermostAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* MattermostAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool MattermostAdapter::connect() {
    if (cfg_.token.empty() || cfg_.url.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    try {
        auto resp = transport->get(
            cfg_.url + "/api/v4/users/me",
            {{"Authorization", "Bearer " + cfg_.token}});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void MattermostAdapter::disconnect() {}

bool MattermostAdapter::send(const std::string& chat_id,
                             const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"channel_id", chat_id},
        {"message", content}
    };

    try {
        auto resp = transport->post_json(
            cfg_.url + "/api/v4/posts",
            {{"Authorization", "Bearer " + cfg_.token},
             {"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void MattermostAdapter::send_typing(const std::string& /*chat_id*/) {
    // Mattermost typing is sent via WebSocket RTM, not REST API.
}

}  // namespace hermes::gateway::platforms
