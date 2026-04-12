// Phase 12 — Slack platform adapter implementation.
#include "slack.hpp"

#include <iomanip>
#include <sstream>

#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

SlackAdapter::SlackAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool SlackAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;
    // TODO(phase-14+): open Socket Mode WebSocket or start RTM.
    return true;
}

void SlackAdapter::disconnect() {}

bool SlackAdapter::send(const std::string& /*chat_id*/,
                        const std::string& /*content*/) {
    // TODO(phase-14+): POST to chat.postMessage
    return true;
}

void SlackAdapter::send_typing(const std::string& /*chat_id*/) {}

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
