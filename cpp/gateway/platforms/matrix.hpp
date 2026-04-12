// Phase 12 — Matrix platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class MatrixAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string homeserver;
        std::string username;
        std::string password;
        std::string access_token;
    };

    explicit MatrixAdapter(Config cfg);

    Platform platform() const override { return Platform::Matrix; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
