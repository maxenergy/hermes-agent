// Phase 12 — API Server platform adapter implementation.
#include "api_server.hpp"

#include <iomanip>
#include <sstream>

#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

ApiServerAdapter::ApiServerAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool ApiServerAdapter::connect() {
    // API server always connects — no external credentials needed.
    // TODO(phase-14+): bind HTTP listener on cfg_.port.
    return true;
}

void ApiServerAdapter::disconnect() {}

bool ApiServerAdapter::send(const std::string& /*chat_id*/,
                            const std::string& /*content*/) {
    // TODO(phase-14+): respond via pending HTTP response.
    return true;
}

void ApiServerAdapter::send_typing(const std::string& /*chat_id*/) {}

bool ApiServerAdapter::verify_hmac_signature(const std::string& secret,
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
