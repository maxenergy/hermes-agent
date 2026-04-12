// Phase 12 — Discord platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class DiscordAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string application_id;
        bool manage_threads = true;
    };

    explicit DiscordAdapter(Config cfg);
    DiscordAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Discord; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Format a Discord user mention from a numeric user ID.
    static std::string format_mention(const std::string& user_id);

    // Create a thread under a channel. Returns true on 2xx.
    bool create_thread(const std::string& channel_id,
                       const std::string& name,
                       int auto_archive_minutes = 1440);

    // Add an emoji reaction to a message.
    bool add_reaction(const std::string& channel_id,
                      const std::string& message_id,
                      const std::string& emoji);

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
};

}  // namespace hermes::gateway::platforms
