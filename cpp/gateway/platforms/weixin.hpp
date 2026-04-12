// Phase 12 — Weixin (WeChat Official Account) platform adapter.
#pragma once

#include <string>

#include <hermes/gateway/gateway_runner.hpp>
#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway::platforms {

// Minimal parsed XML message from Weixin.
struct WeixinMessageEvent {
    std::string from_user;
    std::string to_user;
    std::string msg_type;
    std::string content;
    std::string msg_id;
};

class WeixinAdapter : public BasePlatformAdapter {
public:
    struct Config {
        std::string appid;
        std::string appsecret;
        std::string token;  // verification token
    };

    explicit WeixinAdapter(Config cfg);
    WeixinAdapter(Config cfg, hermes::llm::HttpTransport* transport);

    Platform platform() const override { return Platform::Weixin; }
    bool connect() override;
    void disconnect() override;
    bool send(const std::string& chat_id, const std::string& content) override;
    void send_typing(const std::string& chat_id) override;

    // Parse a Weixin XML message into structured fields.
    static WeixinMessageEvent parse_xml_message(const std::string& xml);

    Config config() const { return cfg_; }

private:
    hermes::llm::HttpTransport* get_transport();
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string wx_access_token_;
};

}  // namespace hermes::gateway::platforms
