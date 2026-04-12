// Phase 12 — Telegram platform adapter.
#pragma once

#include <string>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class TelegramAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        bool use_webhook = false;
        std::string webhook_url;
        std::string reply_to_mode = "first";  // first|all
        std::vector<std::string> fallback_ips;  // GFW bypass
    };

    explicit TelegramAdapter(Config cfg);

    Platform platform() const override { return Platform::Telegram; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Telegram MarkdownV2 escaping.
    static std::string format_markdown_v2(const std::string& text);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
