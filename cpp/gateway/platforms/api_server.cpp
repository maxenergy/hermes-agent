// Phase 12 — API Server platform adapter implementation.
#include "api_server.hpp"

#include <iomanip>
#include <mutex>
#include <sstream>

#include <openssl/hmac.h>

namespace hermes::gateway::platforms {

ApiServerAdapter::ApiServerAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool ApiServerAdapter::connect() {
    // API server always connects — it listens for incoming, doesn't connect out.
    connected_ = true;
    return true;
}

void ApiServerAdapter::disconnect() {
    connected_ = false;
}

bool ApiServerAdapter::send(const std::string& chat_id,
                            const std::string& content) {
    // Store response for pending request in-memory response queue.
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_responses_[chat_id] = content;
    return true;
}

void ApiServerAdapter::send_typing(const std::string& /*chat_id*/) {
    // No typing indicator for API server.
}

std::string ApiServerAdapter::get_pending_response(const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    auto it = pending_responses_.find(chat_id);
    if (it != pending_responses_.end()) {
        std::string resp = std::move(it->second);
        pending_responses_.erase(it);
        return resp;
    }
    return {};
}

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
