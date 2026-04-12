// Phase 12 — Local (stdin/stdout) platform adapter implementation.
#include "local.hpp"

#include <iostream>

namespace hermes::gateway::platforms {

bool LocalAdapter::connect() {
    // Local adapter always succeeds — stdin/stdout are always available.
    return true;
}

void LocalAdapter::disconnect() {}

bool LocalAdapter::send(const std::string& /*chat_id*/,
                        const std::string& content) {
    std::cout << content << std::endl;
    return true;
}

void LocalAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
