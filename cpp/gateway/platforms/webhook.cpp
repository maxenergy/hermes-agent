// Phase 12 — Generic Webhook platform adapter implementation.
#include "webhook.hpp"

#include <iomanip>
#include <sstream>

#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

WebhookAdapter::WebhookAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool WebhookAdapter::connect() {
    // Webhook adapter always succeeds — it's passive.
    return true;
}

void WebhookAdapter::disconnect() {}

bool WebhookAdapter::send(const std::string& /*chat_id*/,
                           const std::string& /*content*/) {
    // TODO(phase-14+): POST JSON to endpoint_url.
    return true;
}

void WebhookAdapter::send_typing(const std::string& /*chat_id*/) {}

bool WebhookAdapter::verify_hmac_signature(const std::string& secret,
                                           const std::string& body,
                                           const std::string& signature) {
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
    return hex_stream.str() == signature;
}

}  // namespace hermes::gateway::platforms
