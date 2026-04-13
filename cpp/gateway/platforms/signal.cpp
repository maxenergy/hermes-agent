// Phase 12 — Signal platform adapter implementation.
#include "signal.hpp"

#include <nlohmann/json.hpp>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

SignalAdapter::SignalAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SignalAdapter::SignalAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SignalAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool SignalAdapter::connect() {
    if (cfg_.http_url.empty() && cfg_.account.empty()) return false;

    // Token-scoped lock: account number is the credential.
    if (!cfg_.account.empty() &&
        !hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.account, {})) {
        return false;
    }

    // Signal REST API does not have a dedicated auth/connect endpoint;
    // we verify connectivity by checking the API is reachable.
    auto* transport = get_transport();
    if (!transport) {
        if (!cfg_.account.empty()) {
            hermes::gateway::release_scoped_lock(
                hermes::gateway::platform_to_string(platform()), cfg_.account);
        }
        return false;
    }

    try {
        auto resp = transport->get(
            cfg_.http_url + "/v1/about", {});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void SignalAdapter::disconnect() {
    if (!cfg_.account.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.account);
    }
}

bool SignalAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"message", content},
        {"number", cfg_.account},
        {"recipients", nlohmann::json::array({chat_id})}
    };

    try {
        auto resp = transport->post_json(
            cfg_.http_url + "/v2/send",
            {{"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void SignalAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string SignalAdapter::normalize_identifier(const std::string& id) {
    return id;
}

}  // namespace hermes::gateway::platforms
