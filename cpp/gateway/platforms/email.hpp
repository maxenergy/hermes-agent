// Phase 12 — Email platform adapter.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

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

    // IMAP protocol command formatters (pure string helpers, no IO).
    static std::string imap_login_command(const std::string& tag,
                                          const std::string& user,
                                          const std::string& password);
    static std::string imap_select_command(const std::string& tag,
                                           const std::string& mailbox);
    static std::string imap_idle_command(const std::string& tag);
    static std::string imap_done_command();
    static std::string imap_uid_fetch_command(const std::string& tag,
                                              const std::string& uid_spec);

    // Parse an IMAP EXISTS notification: "* 42 EXISTS" -> 42.
    // Returns -1 if the line is not an EXISTS notification.
    static long long parse_exists_notification(const std::string& line);

    // Background IMAP IDLE receive loop.
    void start_imap_idle_loop(
        std::function<void(const std::string&)> message_handler);
    void stop_imap_idle_loop();
    bool idle_loop_running() const { return idle_running_.load(); }

    Config config() const { return cfg_; }

private:
    Config cfg_;
    std::atomic<bool> idle_running_{false};
    std::thread idle_thread_;
};

}  // namespace hermes::gateway::platforms
