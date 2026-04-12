// Phase 12 — SMS (Twilio) platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class SmsAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string twilio_account_sid;
        std::string twilio_auth_token;
        std::string from_number;
    };

    explicit SmsAdapter(Config cfg);

    Platform platform() const override { return Platform::Sms; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
