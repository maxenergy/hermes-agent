// Phase 12 — Generic Webhook platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class WebhookAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string signature_secret;
        std::string endpoint_url;
    };

    explicit WebhookAdapter(Config cfg);
    WebhookAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Webhook; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Verify HMAC-SHA256 signature.
    static bool verify_hmac_signature(const std::string& secret,
                                      const std::string& body,
                                      const std::string& signature);

    // Compute HMAC-SHA256 hex digest.
    static std::string compute_hmac(const std::string& secret,
                                    const std::string& body);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
};

}  // namespace hermes::gateway::platforms
