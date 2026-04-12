// Phase 12 — WhatsApp platform adapter implementation.
#include "whatsapp.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

WhatsAppAdapter::WhatsAppAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WhatsAppAdapter::WhatsAppAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WhatsAppAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WhatsAppAdapter::connect() {
    if (cfg_.session_dir.empty() && cfg_.phone.empty()) return false;
    // WhatsApp uses whatsmeow bridge which requires WebSocket for receiving.
    // connect() notes this; send() can use HTTP API.
    return true;
}

void WhatsAppAdapter::disconnect() {}

bool WhatsAppAdapter::send(const std::string& chat_id,
                           const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // WhatsApp Cloud API endpoint.
    std::string jid = resolve_jid(chat_id);
    nlohmann::json payload = {
        {"messaging_product", "whatsapp"},
        {"to", jid},
        {"type", "text"},
        {"text", {{"body", content}}}
    };

    try {
        auto resp = transport->post_json(
            "https://graph.facebook.com/v17.0/" + cfg_.phone + "/messages",
            {{"Content-Type", "application/json"}},
            payload.dump());
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void WhatsAppAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string WhatsAppAdapter::resolve_jid(const std::string& phone) {
    return phone + "@s.whatsapp.net";
}

}  // namespace hermes::gateway::platforms
