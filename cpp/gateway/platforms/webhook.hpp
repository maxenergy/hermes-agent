// Phase 12 — Generic Webhook platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class WebhookAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string signature_secret;
        std::string endpoint_url;
    };

    explicit WebhookAdapter(Config cfg);

    Platform platform() const override { return Platform::Webhook; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Verify HMAC-SHA256 signature.
    static bool verify_hmac_signature(const std::string& secret,
                                      const std::string& body,
                                      const std::string& signature);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
