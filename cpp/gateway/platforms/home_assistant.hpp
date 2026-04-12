// Phase 12 — Home Assistant platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class HomeAssistantAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string hass_token;
        std::string hass_url = "http://localhost:8123";
    };

    explicit HomeAssistantAdapter(Config cfg);
    HomeAssistantAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::HomeAssistant; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
};

}  // namespace hermes::gateway::platforms
