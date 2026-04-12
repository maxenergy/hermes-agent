// Phase 12 — Signal platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class SignalAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string http_url;  // signal-cli REST API URL
        std::string account;   // phone number or UUID
    };

    explicit SignalAdapter(Config cfg);
    SignalAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Signal; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Resolve UUID+phone alias to canonical form.
    static std::string normalize_identifier(const std::string& id);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
};

}  // namespace hermes::gateway::platforms
