// Phase 12 — DingTalk platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class DingTalkAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string client_id;
        std::string client_secret;
    };

    explicit DingTalkAdapter(Config cfg);
    DingTalkAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::DingTalk; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Extract @mention user IDs from DingTalk message text.
    static std::string extract_mention(const std::string& text);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string access_token_;
};

}  // namespace hermes::gateway::platforms
