// Phase 12 — SMS (Twilio) platform adapter implementation.
#include "sms.hpp"

namespace hermes::gateway::platforms {

SmsAdapter::SmsAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool SmsAdapter::connect() {
    if (cfg_.twilio_account_sid.empty() || cfg_.twilio_auth_token.empty())
        return false;
    // TODO(phase-14+): verify Twilio credentials.
    return true;
}

void SmsAdapter::disconnect() {}

bool SmsAdapter::send(const std::string& /*chat_id*/,
                      const std::string& /*content*/) {
    // TODO(phase-14+): POST to Twilio Messages API.
    return true;
}

void SmsAdapter::send_typing(const std::string& /*chat_id*/) {
    // SMS has no typing indicator.
}

}  // namespace hermes::gateway::platforms
