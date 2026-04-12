// Phase 12 — Discord platform adapter implementation.
#include "discord.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

DiscordAdapter::DiscordAdapter(Config cfg) : cfg_(std::move(cfg)) {}

DiscordAdapter::DiscordAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* DiscordAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool DiscordAdapter::connect() {
    if (cfg_.bot_token.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    std::string url = "https://discord.com/api/v10/users/@me";
    try {
        auto resp = transport->get(
            url, {{"Authorization", "Bot " + cfg_.bot_token}});
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("id")) return false;

        connected_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void DiscordAdapter::disconnect() {
    connected_ = false;
}

bool DiscordAdapter::send(const std::string& chat_id,
                          const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    std::string url = "https://discord.com/api/v10/channels/" + chat_id + "/messages";
    nlohmann::json payload = {{"content", content}};

    try {
        auto resp = transport->post_json(
            url,
            {{"Authorization", "Bot " + cfg_.bot_token},
             {"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void DiscordAdapter::send_typing(const std::string& chat_id) {
    auto* transport = get_transport();
    if (!transport) return;

    std::string url = "https://discord.com/api/v10/channels/" + chat_id + "/typing";
    try {
        transport->post_json(
            url,
            {{"Authorization", "Bot " + cfg_.bot_token}},
            "");
    } catch (...) {
        // Best-effort.
    }
}

std::string DiscordAdapter::format_mention(const std::string& user_id) {
    return "<@" + user_id + ">";
}

}  // namespace hermes::gateway::platforms
