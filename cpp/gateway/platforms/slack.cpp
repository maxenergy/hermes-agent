// Phase 12 — Slack platform adapter implementation.
#include "slack.hpp"

#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

SlackAdapter::SlackAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SlackAdapter::SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SlackAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool SlackAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    try {
        auto resp = transport->post_json(
            "https://slack.com/api/auth.test",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            "{}");
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return false;

        connected_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void SlackAdapter::disconnect() {
    connected_ = false;
}

bool SlackAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"channel", chat_id},
        {"text", content}
    };

    try {
        auto resp = transport->post_json(
            "https://slack.com/api/chat.postMessage",
            {{"Authorization", "Bearer " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        return body.value("ok", false);
    } catch (...) {
        return false;
    }
}

void SlackAdapter::send_typing(const std::string& /*chat_id*/) {
    // Slack typing indicators are sent via WebSocket (RTM/Socket Mode),
    // not via the Web API. No-op for HTTP-only send path.
}

std::string SlackAdapter::compute_slack_signature(
    const std::string& signing_secret,
    const std::string& timestamp,
    const std::string& body) {
    std::string base_string = "v0:" + timestamp + ":" + body;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         signing_secret.data(),
         static_cast<int>(signing_secret.size()),
         reinterpret_cast<const unsigned char*>(base_string.data()),
         base_string.size(),
         digest,
         &digest_len);

    std::ostringstream hex_stream;
    hex_stream << "v0=";
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2)
                   << static_cast<int>(digest[i]);
    }
    return hex_stream.str();
}

}  // namespace hermes::gateway::platforms
