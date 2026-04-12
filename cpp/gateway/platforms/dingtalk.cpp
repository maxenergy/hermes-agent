// Phase 12 — DingTalk platform adapter implementation.
#include "dingtalk.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

DingTalkAdapter::DingTalkAdapter(Config cfg) : cfg_(std::move(cfg)) {}

DingTalkAdapter::DingTalkAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* DingTalkAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool DingTalkAdapter::connect() {
    if (cfg_.client_id.empty() || cfg_.client_secret.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // Obtain access_token via OAuth.
    nlohmann::json payload = {
        {"appKey", cfg_.client_id},
        {"appSecret", cfg_.client_secret}
    };

    try {
        auto resp = transport->post_json(
            "https://api.dingtalk.com/v1.0/oauth2/accessToken",
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("accessToken")) return false;
        access_token_ = body["accessToken"].get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

void DingTalkAdapter::disconnect() {
    access_token_.clear();
}

bool DingTalkAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // Send via DingTalk Robot message to conversation.
    nlohmann::json payload = {
        {"msgtype", "text"},
        {"text", {{"content", content}}}
    };

    try {
        // If we have an access_token (OAuth flow), use the internal API.
        // Otherwise fall back to webhook-style send.
        std::string url = "https://api.dingtalk.com/v1.0/robot/oToMessages/batchSend";
        nlohmann::json batch_payload = {
            {"robotCode", cfg_.client_id},
            {"userIds", nlohmann::json::array({chat_id})},
            {"msgKey", "sampleText"},
            {"msgParam", nlohmann::json({{"content", content}}).dump()}
        };

        auto resp = transport->post_json(
            url,
            {{"x-acs-dingtalk-access-token", access_token_},
             {"Content-Type", "application/json"}},
            batch_payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void DingTalkAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string DingTalkAdapter::extract_mention(const std::string& text) {
    auto pos = text.find('@');
    if (pos == std::string::npos) return {};
    auto end = text.find(' ', pos);
    if (end == std::string::npos) end = text.size();
    return text.substr(pos + 1, end - pos - 1);
}

}  // namespace hermes::gateway::platforms
