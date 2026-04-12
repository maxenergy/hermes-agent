// Phase 12 — Telegram platform adapter implementation.
#include "telegram.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

TelegramAdapter::TelegramAdapter(Config cfg) : cfg_(std::move(cfg)) {}

TelegramAdapter::TelegramAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* TelegramAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool TelegramAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    std::string url = "https://api.telegram.org/bot" + cfg_.bot_token + "/getMe";
    try {
        auto resp = transport->get(url, {});
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.value("ok", false)) return false;

        if (body.contains("result") && body["result"].contains("username")) {
            bot_username_ = body["result"]["username"].get<std::string>();
        }
        connected_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void TelegramAdapter::disconnect() {
    connected_ = false;
}

bool TelegramAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    std::string url = "https://api.telegram.org/bot" + cfg_.bot_token + "/sendMessage";
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"text", format_markdown_v2(content)},
        {"parse_mode", "MarkdownV2"}
    };

    try {
        auto resp = transport->post_json(
            url, {{"Content-Type", "application/json"}}, payload.dump());
        if (resp.status_code != 200) return false;
        auto body = nlohmann::json::parse(resp.body);
        return body.value("ok", false);
    } catch (...) {
        return false;
    }
}

void TelegramAdapter::send_typing(const std::string& chat_id) {
    auto* transport = get_transport();
    if (!transport) return;

    std::string url = "https://api.telegram.org/bot" + cfg_.bot_token + "/sendChatAction";
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"action", "typing"}
    };

    try {
        transport->post_json(
            url, {{"Content-Type", "application/json"}}, payload.dump());
    } catch (...) {
        // Best-effort; ignore errors for typing indicator.
    }
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
