// Phase 12 — Feishu (Lark) platform adapter implementation.
#include "feishu.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

FeishuAdapter::FeishuAdapter(Config cfg) : cfg_(std::move(cfg)) {}

FeishuAdapter::FeishuAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* FeishuAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool FeishuAdapter::connect() {
    if (cfg_.app_id.empty() || cfg_.app_secret.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // Obtain tenant_access_token.
    nlohmann::json payload = {
        {"app_id", cfg_.app_id},
        {"app_secret", cfg_.app_secret}
    };

    try {
        auto resp = transport->post_json(
            "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (body.value("code", -1) != 0) return false;
        if (!body.contains("tenant_access_token")) return false;
        tenant_access_token_ = body["tenant_access_token"].get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

void FeishuAdapter::disconnect() {
    tenant_access_token_.clear();
}

bool FeishuAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    nlohmann::json payload = {
        {"receive_id", chat_id},
        {"msg_type", "text"},
        {"content", nlohmann::json({{"text", content}}).dump()}
    };

    try {
        auto resp = transport->post_json(
            "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
            {{"Authorization", "Bearer " + tenant_access_token_},
             {"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void FeishuAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string FeishuAdapter::build_card_message(const std::string& title,
                                              const std::string& content) {
    return R"({"msg_type":"interactive","card":{"header":{"title":{"tag":"plain_text","content":")"
           + title + R"("}}},"elements":[{"tag":"div","text":{"tag":"plain_text","content":")"
           + content + R"("}}]})";
}

}  // namespace hermes::gateway::platforms
