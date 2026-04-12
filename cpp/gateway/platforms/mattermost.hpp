// Phase 12 — Mattermost platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class MattermostAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string token;
        std::string url;
    };

    explicit MattermostAdapter(Config cfg);

    Platform platform() const override { return Platform::Mattermost; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
