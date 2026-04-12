// Phase 12 — Local (stdin/stdout) platform adapter for testing.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class LocalAdapter : public BasePlatformAdapter {
public:
    LocalAdapter() = default;

    Platform platform() const override { return Platform::Local; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;
};

}  // namespace hermes::gateway::platforms
