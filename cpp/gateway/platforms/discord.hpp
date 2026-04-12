// Phase 12 — Discord platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class DiscordAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string application_id;
        bool manage_threads = true;
    };

    explicit DiscordAdapter(Config cfg);

    Platform platform() const override { return Platform::Discord; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Format a Discord user mention from a numeric user ID.
    static std::string format_mention(const std::string& user_id);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
