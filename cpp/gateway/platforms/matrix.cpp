// Phase 12 — Matrix platform adapter implementation.
#include "matrix.hpp"

namespace hermes::gateway::platforms {

MatrixAdapter::MatrixAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool MatrixAdapter::connect() {
    if (cfg_.homeserver.empty()) return false;
    if (cfg_.access_token.empty() && (cfg_.username.empty() || cfg_.password.empty()))
        return false;
    // TODO(phase-14+): login and /sync loop.
    return true;
}

void MatrixAdapter::disconnect() {}

bool MatrixAdapter::send(const std::string& /*chat_id*/,
                         const std::string& /*content*/) {
    return true;
}

void MatrixAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
