// Phase 12 — WeCom (WeChat Work) platform adapter implementation.
#include "wecom.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

WeComAdapter::WeComAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WeComAdapter::WeComAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WeComAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WeComAdapter::connect() {
    if (cfg_.bot_id.empty() && cfg_.webhook_url.empty()) return false;
    return true;
}

void WeComAdapter::disconnect() {}

bool WeComAdapter::send(const std::string& /*chat_id*/,
                        const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // Use webhook URL if available (group robot).
    if (!cfg_.webhook_url.empty()) {
        std::string body = build_webhook_message(content);
        try {
            auto resp = transport->post_json(
                cfg_.webhook_url,
                {{"Content-Type", "application/json"}},
                body);
            return resp.status_code >= 200 && resp.status_code < 300;
        } catch (...) {
            return false;
        }
    }

    return false;
}

void WeComAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string WeComAdapter::build_webhook_message(const std::string& content) {
    return R"({"msgtype":"text","text":{"content":")" + content + R"("}})";
}

}  // namespace hermes::gateway::platforms
