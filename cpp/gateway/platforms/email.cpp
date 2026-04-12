// Phase 12 — Email platform adapter implementation.
#include "email.hpp"

namespace hermes::gateway::platforms {

EmailAdapter::EmailAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool EmailAdapter::connect() {
    if (cfg_.address.empty() || cfg_.password.empty()) return false;
    // TODO(phase-14+): IMAP connect + IDLE.
    return true;
}

void EmailAdapter::disconnect() {}

bool EmailAdapter::send(const std::string& /*chat_id*/,
                        const std::string& /*content*/) {
    // TODO(phase-14+): SMTP send.
    return true;
}

void EmailAdapter::send_typing(const std::string& /*chat_id*/) {
    // Email has no typing indicator.
}

std::string EmailAdapter::build_mime_message(const std::string& from,
                                             const std::string& to,
                                             const std::string& subject,
                                             const std::string& body) {
    std::string msg;
    msg += "From: " + from + "\r\n";
    msg += "To: " + to + "\r\n";
    msg += "Subject: " + subject + "\r\n";
    msg += "MIME-Version: 1.0\r\n";
    msg += "Content-Type: text/plain; charset=UTF-8\r\n";
    msg += "Content-Transfer-Encoding: 8bit\r\n";
    msg += "\r\n";
    msg += body;
    return msg;
}

}  // namespace hermes::gateway::platforms
