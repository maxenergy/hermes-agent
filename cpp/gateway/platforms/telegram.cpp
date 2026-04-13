// Phase 12 — Telegram platform adapter implementation.
#include "telegram.hpp"

#include <nlohmann/json.hpp>

#include <hermes/gateway/status.hpp>

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

    // Token-scoped lock: ensure no other profile is running with the
    // same bot token.
    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            cfg_.bot_token, {})) {
        return false;
    }

    auto* transport = get_transport();
    if (!transport) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
        return false;
    }

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
    if (connected_ || !cfg_.bot_token.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()), cfg_.bot_token);
    }
    connected_ = false;
}

bool TelegramAdapter::set_reaction(const std::string& chat_id,
                                   long long message_id,
                                   const std::string& emoji) {
    auto* transport = get_transport();
    if (!transport) return false;
    std::string url =
        "https://api.telegram.org/bot" + cfg_.bot_token + "/setMessageReaction";
    nlohmann::json payload = {
        {"chat_id", chat_id},
        {"message_id", message_id},
        {"reaction", nlohmann::json::array({nlohmann::json{
                         {"type", "emoji"}, {"emoji", emoji}}})},
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

bool TelegramAdapter::set_my_commands(
    const std::vector<std::pair<std::string, std::string>>& commands) {
    auto* transport = get_transport();
    if (!transport) return false;
    if (cfg_.bot_token.empty()) return false;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [name, desc] : commands) {
        if (name.empty()) continue;
        std::string d = desc;
        if (d.size() > 256) d.resize(256);
        if (d.empty()) d = name;
        arr.push_back({{"command", name}, {"description", d}});
    }

    nlohmann::json payload = {{"commands", arr}};
    std::string url =
        "https://api.telegram.org/bot" + cfg_.bot_token + "/setMyCommands";
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

std::optional<long long> TelegramAdapter::parse_forum_topic(
    const nlohmann::json& message) {
    if (message.contains("message_thread_id") &&
        message["message_thread_id"].is_number_integer()) {
        return message["message_thread_id"].get<long long>();
    }
    return std::nullopt;
}

std::optional<std::string> TelegramAdapter::parse_media_group_id(
    const nlohmann::json& message) {
    if (message.contains("media_group_id") &&
        message["media_group_id"].is_string()) {
        return message["media_group_id"].get<std::string>();
    }
    return std::nullopt;
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
