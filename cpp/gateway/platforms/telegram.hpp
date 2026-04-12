// Phase 12 — Telegram platform adapter.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

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
    // Constructor with injectable transport (for testing).
    TelegramAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Telegram; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Telegram MarkdownV2 escaping.
    static std::string format_markdown_v2(const std::string& text);

    // Send an emoji reaction to a Telegram message via setMessageReaction.
    bool set_reaction(const std::string& chat_id, long long message_id,
                      const std::string& emoji);

    // Extract the message_thread_id (Telegram forum topic) if present.
    static std::optional<long long> parse_forum_topic(
        const nlohmann::json& message);

    // Extract the media_group_id (album batching) if present.
    static std::optional<std::string> parse_media_group_id(
        const nlohmann::json& message);

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
    std::string bot_username_;
};

}  // namespace hermes::gateway::platforms
