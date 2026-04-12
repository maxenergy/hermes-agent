// Phase 12 — Discord platform adapter implementation.
#include "discord.hpp"

namespace hermes::gateway::platforms {

DiscordAdapter::DiscordAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool DiscordAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;
    // TODO(phase-14+): connect to Discord Gateway WebSocket.
    return true;
}

void DiscordAdapter::disconnect() {}

bool DiscordAdapter::send(const std::string& /*chat_id*/,
                          const std::string& /*content*/) {
    // TODO(phase-14+): POST to /channels/{id}/messages
    return true;
}

void DiscordAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string DiscordAdapter::format_mention(const std::string& user_id) {
    return "<@" + user_id + ">";
}

}  // namespace hermes::gateway::platforms
