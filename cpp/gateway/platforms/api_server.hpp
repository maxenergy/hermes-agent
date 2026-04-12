// Phase 12 — API Server platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class ApiServerAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string hmac_secret;
        int port = 8080;
    };

    explicit ApiServerAdapter(Config cfg);

    Platform platform() const override { return Platform::ApiServer; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Verify HMAC-SHA256 signature on incoming request body.
    static bool verify_hmac_signature(const std::string& secret,
                                      const std::string& body,
                                      const std::string& signature);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
