// Phase 12 — Slack platform adapter.
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

class SlackAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string bot_token;
        std::string signing_secret;
        std::string app_token;  // for Socket Mode
    };

    explicit SlackAdapter(Config cfg);
    SlackAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Slack; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Compute Slack request signature: HMAC-SHA256 of "v0:{timestamp}:{body}".
    static std::string compute_slack_signature(const std::string& signing_secret,
                                               const std::string& timestamp,
                                               const std::string& body);

    // Extract thread_ts from an incoming Slack event payload — present
    // only on replies inside a thread.
    static std::optional<std::string> parse_thread_ts(
        const nlohmann::json& event);

    // Send a message as a reply inside a thread.
    bool send_thread_reply(const std::string& chat_id,
                           const std::string& thread_ts,
                           const std::string& content);

    // Upload a file to a channel via the Slack files.upload endpoint.
    bool upload_file(const std::string& chat_id,
                     const std::string& filename,
                     const std::string& content,
                     const std::string& initial_comment = "");

    Config config() const { return cfg_; }
    bool connected() const { return connected_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    bool connected_ = false;
};

}  // namespace hermes::gateway::platforms
