// Phase 12 — Telegram platform adapter implementation.
#include "telegram.hpp"

namespace hermes::gateway::platforms {

TelegramAdapter::TelegramAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool TelegramAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;
    // TODO(phase-14+): actual Bot API connection via HTTPS long-polling or webhook.
    return true;
}

void TelegramAdapter::disconnect() {
    // TODO(phase-14+): close polling / deregister webhook.
}

bool TelegramAdapter::send(const std::string& /*chat_id*/,
                           const std::string& /*content*/) {
    // TODO(phase-14+): POST to https://api.telegram.org/bot<token>/sendMessage
    return true;
}

void TelegramAdapter::send_typing(const std::string& /*chat_id*/) {
    // TODO(phase-14+): sendChatAction typing
}

// Escape all MarkdownV2 special characters.
std::string TelegramAdapter::format_markdown_v2(const std::string& text) {
    static const std::string specials = R"(_*[]()~`>#+-=|{}.!\)";
    std::string result;
    result.reserve(text.size() * 2);
    for (char ch : text) {
        if (specials.find(ch) != std::string::npos) {
            result += '\\';
        }
        result += ch;
    }
    return result;
}

}  // namespace hermes::gateway::platforms
