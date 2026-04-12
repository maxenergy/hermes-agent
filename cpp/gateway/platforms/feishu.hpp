// Phase 12 — Feishu (Lark) platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class FeishuAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string app_id;
        std::string app_secret;
    };

    explicit FeishuAdapter(Config cfg);

    Platform platform() const override { return Platform::Feishu; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Build Feishu interactive card message JSON (stub).
    static std::string build_card_message(const std::string& title,
                                          const std::string& content);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
