// Phase 12 — Local (stdin/stdout) platform adapter implementation.
#include "local.hpp"

namespace hermes::gateway::platforms {

bool LocalAdapter::connect() {
    // Local adapter always succeeds — stdin/stdout are always available.
    return true;
}

void LocalAdapter::disconnect() {}

bool LocalAdapter::send(const std::string& /*chat_id*/,
                        const std::string& /*content*/) {
    // TODO(phase-14+): write to stdout.
    return true;
}

void LocalAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
