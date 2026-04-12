// Phase 12 — WeCom (WeChat Work) platform adapter implementation.
#include "wecom.hpp"

namespace hermes::gateway::platforms {

WeComAdapter::WeComAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool WeComAdapter::connect() {
    if (cfg_.bot_id.empty() && cfg_.webhook_url.empty()) return false;
    // TODO(phase-14+): verify WeCom credentials.
    return true;
}

void WeComAdapter::disconnect() {}

bool WeComAdapter::send(const std::string& /*chat_id*/,
                        const std::string& /*content*/) {
    return true;
}

void WeComAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string WeComAdapter::build_webhook_message(const std::string& content) {
    return R"({"msgtype":"text","text":{"content":")" + content + R"("}})";
}

}  // namespace hermes::gateway::platforms
