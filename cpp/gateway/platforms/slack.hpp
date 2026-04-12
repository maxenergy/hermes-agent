// Phase 12 — Slack platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class SlackAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string signing_secret;
        std::string app_token;  // for Socket Mode
    };

    explicit SlackAdapter(Config cfg);
    SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Slack; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Compute Slack request signature: HMAC-SHA256 of "v0:{timestamp}:{body}".
    static std::string compute_slack_signature(const std::string& signing_secret,
                                               const std::string& timestamp,
                                               const std::string& body);

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
};

}  // namespace hermes::gateway::platforms
