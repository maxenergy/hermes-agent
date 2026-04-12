// Phase 12 — Generic Webhook platform adapter implementation.
#include "webhook.hpp"

#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

WebhookAdapter::WebhookAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WebhookAdapter::WebhookAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WebhookAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WebhookAdapter::connect() {
    // Webhook adapter always succeeds — it's passive.
    return true;
}

void WebhookAdapter::disconnect() {}

bool WebhookAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    if (cfg_.endpoint_url.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"content", content}
    };
    std::string body = payload.dump();

    std::unordered_map<std::string, std::string> headers = {
        {"Content-Type", "application/json"}
    };

    // Add HMAC signature header if secret is configured.
    if (!cfg_.signature_secret.empty()) {
        headers["X-Hermes-Signature"] = compute_hmac(cfg_.signature_secret, body);
    }

    try {
        auto resp = transport->post_json(cfg_.endpoint_url, headers, body);
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void WebhookAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string WebhookAdapter::compute_hmac(const std::string& secret,
                                         const std::string& body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         secret.data(),
         static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(body.data()),
         body.size(),
         digest,
         &digest_len);

    std::ostringstream hex_stream;
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex_stream << std::hex << std::setfill('0') << std::setw(2)
                   << static_cast<int>(digest[i]);
    }
    return hex_stream.str();
}

bool WebhookAdapter::verify_hmac_signature(const std::string& secret,
                                           const std::string& body,
                                           const std::string& signature) {
    return compute_hmac(secret, body) == signature;
}

}  // namespace hermes::gateway::platforms
