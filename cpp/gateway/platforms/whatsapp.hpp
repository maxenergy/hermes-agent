// Phase 12 — WhatsApp platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class WhatsAppAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string session_dir;
        std::string phone;
    };

    explicit WhatsAppAdapter(Config cfg);

    Platform platform() const override { return Platform::WhatsApp; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Resolve JID/LID alias (stub).
    static std::string resolve_jid(const std::string& phone);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
