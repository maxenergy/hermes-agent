// Phase 12 — WhatsApp platform adapter implementation.
#include "whatsapp.hpp"

namespace hermes::gateway::platforms {

WhatsAppAdapter::WhatsAppAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool WhatsAppAdapter::connect() {
    if (cfg_.session_dir.empty() && cfg_.phone.empty()) return false;
    // TODO(phase-14+): connect via whatsmeow bridge.
    return true;
}

void WhatsAppAdapter::disconnect() {}

bool WhatsAppAdapter::send(const std::string& /*chat_id*/,
                           const std::string& /*content*/) {
    return true;
}

void WhatsAppAdapter::send_typing(const std::string& /*chat_id*/) {}

std::string WhatsAppAdapter::resolve_jid(const std::string& phone) {
    // Stub: return phone@s.whatsapp.net format.
    return phone + "@s.whatsapp.net";
}

}  // namespace hermes::gateway::platforms
