// Phase 12 — Email platform adapter implementation.
#include "email.hpp"

#include <array>
#include <cstdio>

namespace hermes::gateway::platforms {

EmailAdapter::EmailAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool EmailAdapter::connect() {
    if (cfg_.address.empty() || cfg_.password.empty()) return false;
    // IMAP IDLE connection for receiving is not yet implemented; send via
    // sendmail/SMTP is available.
    return true;
}

void EmailAdapter::disconnect() {}

bool EmailAdapter::send(const std::string& chat_id,
                        const std::string& content) {
    // chat_id is the recipient email address.
    std::string mime = build_mime_message(cfg_.address, chat_id, "Hermes", content);

    // Try sendmail first (available on most Unix systems).
    std::string cmd = "sendmail -t 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;

    std::fwrite(mime.data(), 1, mime.size(), pipe);
    int rc = pclose(pipe);
    return rc == 0;
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
