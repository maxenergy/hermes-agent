// Phase 12 — WeCom (WeChat Work) platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class WeComAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_id;
        std::string message_token;
        std::string webhook_url;
    };

    explicit WeComAdapter(Config cfg);
    WeComAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::WeCom; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Build webhook message JSON payload.
    static std::string build_webhook_message(const std::string& content);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
};

}  // namespace hermes::gateway::platforms
