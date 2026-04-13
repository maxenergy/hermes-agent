// Phase 12 — WhatsApp platform adapter implementation.
#include "whatsapp.hpp"

#include <nlohmann/json.hpp>

#include <hermes/gateway/status.hpp>

namespace hermes::gateway::platforms {

namespace {
std::string whatsapp_lock_identity(const WhatsAppAdapter::Config& cfg) {
    // Prefer phone-number-id (graph API) since it's the credential that
    // Meta scopes per WABA.  Fall back to session_dir for whatsmeow.
    if (!cfg.phone.empty()) return "phone:" + cfg.phone;
    return "session:" + cfg.session_dir;
}
}  // namespace

WhatsAppAdapter::WhatsAppAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WhatsAppAdapter::WhatsAppAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WhatsAppAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WhatsAppAdapter::connect() {
    if (cfg_.session_dir.empty() && cfg_.phone.empty()) return false;
    // Token-scoped lock: phone+session-dir is the credential.
    if (!hermes::gateway::acquire_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            whatsapp_lock_identity(cfg_), {})) {
        return false;
    }
    // WhatsApp uses whatsmeow bridge which requires WebSocket for receiving.
    // connect() notes this; send() can use HTTP API.
    return true;
}

void WhatsAppAdapter::disconnect() {
    if (!cfg_.session_dir.empty() || !cfg_.phone.empty()) {
        hermes::gateway::release_scoped_lock(
            hermes::gateway::platform_to_string(platform()),
            whatsapp_lock_identity(cfg_));
    }
}

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
