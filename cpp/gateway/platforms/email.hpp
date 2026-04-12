// Phase 12 — Email platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>

namespace hermes::gateway::platforms {

class EmailAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string address;
        std::string password;
        std::string imap_host;
        int imap_port = 993;
        std::string smtp_host;
        int smtp_port = 587;
    };

    explicit EmailAdapter(Config cfg);

    Platform platform() const override { return Platform::Email; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Build a basic MIME text/plain message.
    static std::string build_mime_message(const std::string& from,
                                          const std::string& to,
                                          const std::string& subject,
                                          const std::string& body);

    Config config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace hermes::gateway::platforms
