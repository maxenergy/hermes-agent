// Phase 12 — SMS (Twilio) platform adapter implementation.
#include "sms.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

SmsAdapter::SmsAdapter(Config cfg) : cfg_(std::move(cfg)) {}

SmsAdapter::SmsAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* SmsAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool SmsAdapter::connect() {
    if (cfg_.twilio_account_sid.empty() || cfg_.twilio_auth_token.empty())
        return false;

    // Verify Twilio credentials by hitting the account info endpoint.
    auto* transport = get_transport();
    if (!transport) return false;

    try {
        auto resp = transport->get(
            "https://api.twilio.com/2010-04-01/Accounts/" +
                cfg_.twilio_account_sid + ".json",
            {{"Authorization",
              "Basic " + cfg_.twilio_account_sid + ":" + cfg_.twilio_auth_token}});
        return resp.status_code == 200;
    } catch (...) {
        return false;
    }
}

void SmsAdapter::disconnect() {}

bool SmsAdapter::send(const std::string& chat_id,
                      const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // Twilio Messages API uses form-urlencoded, but we send JSON for simplicity.
    // In production, this would be form-encoded. Using JSON endpoint as proxy.
    std::string url = "https://api.twilio.com/2010-04-01/Accounts/" +
                      cfg_.twilio_account_sid + "/Messages.json";

    // Twilio expects form-urlencoded; construct the body.
    std::string body = "To=" + chat_id + "&From=" + cfg_.from_number +
                       "&Body=" + content;

    try {
        auto resp = transport->post_json(
            url,
            {{"Authorization",
              "Basic " + cfg_.twilio_account_sid + ":" + cfg_.twilio_auth_token},
             {"Content-Type", "application/x-www-form-urlencoded"}},
            body);
        return resp.status_code >= 200 && resp.status_code < 300;
    } catch (...) {
        return false;
    }
}

void SmsAdapter::send_typing(const std::string& /*chat_id*/) {
    // SMS has no typing indicator.
}

}  // namespace hermes::gateway::platforms
