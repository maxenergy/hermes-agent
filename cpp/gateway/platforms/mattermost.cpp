// Phase 12 — Mattermost platform adapter implementation.
#include "mattermost.hpp"

namespace hermes::gateway::platforms {

MattermostAdapter::MattermostAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool MattermostAdapter::connect() {
    if (cfg_.token.empty() || cfg_.url.empty()) return false;
    // TODO(phase-14+): WebSocket RTM connection.
    return true;
}

void MattermostAdapter::disconnect() {}

bool MattermostAdapter::send(const std::string& /*chat_id*/,
                             const std::string& /*content*/) {
    return true;
}

void MattermostAdapter::send_typing(const std::string& /*chat_id*/) {}

}  // namespace hermes::gateway::platforms
