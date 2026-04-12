// Phase 12 — Feishu (Lark) platform adapter implementation.
#include "feishu.hpp"

namespace hermes::gateway::platforms {

FeishuAdapter::FeishuAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool FeishuAdapter::connect() {
    if (cfg_.app_id.empty() || cfg_.app_secret.empty()) return false;
    // TODO(phase-14+): obtain tenant_access_token.
    return true;
}

void FeishuAdapter::disconnect() {}

bool FeishuAdapter::send(const std::string& /*chat_id*/,
                         const std::string& /*content*/) {
    return true;
}

void FeishuAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string FeishuAdapter::build_card_message(const std::string& title,
                                              const std::string& content) {
    // Stub: return a minimal interactive card JSON structure.
    return R"({"msg_type":"interactive","card":{"header":{"title":{"tag":"plain_text","content":")"
           + title + R"("}}},"elements":[{"tag":"div","text":{"tag":"plain_text","content":")"
           + content + R"("}}]})";
}

}  // namespace hermes::gateway::platforms
