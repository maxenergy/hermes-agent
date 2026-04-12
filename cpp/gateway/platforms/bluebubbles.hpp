// Phase 12 — BlueBubbles (iMessage) platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class BlueBubblesAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string server_url;
        std::string password;
    };

    explicit BlueBubblesAdapter(Config cfg);

    Platform platform() const override { return Platform::BlueBubbles; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
