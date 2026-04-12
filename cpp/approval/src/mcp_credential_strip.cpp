#include "hermes/approval/mcp_credential_strip.hpp"

#include "hermes/core/redact.hpp"

#include <array>
#include <regex>
#include <string>

namespace hermes::approval {

namespace {

constexpr const char* kReplacement = "[REDACTED]";

// MCP-specific patterns layered on top of hermes::core::redact_secrets.
// The core redactor handles sk-*, ghp_*, Bearer, token=, key=, password=,
// secret=. The patterns below add cloud / SSH / API-header / private-key
// shapes that the core layer doesn't know about.
const std::array<std::regex, 11>& mcp_patterns() {
    static const std::array<std::regex, 11> table{
        // GitHub OAuth tokens (gho_), refresh tokens (ghr_), user tokens (ghu_)
        std::regex(R"(gh[oru]_[A-Za-z0-9]{20,})"),
        // AWS access key id
        std::regex(R"(AKIA[0-9A-Z]{16})"),
        // AWS session/secret long form
        std::regex(R"(ASIA[0-9A-Z]{16})"),
        // SSH public key body (ssh-rsa AAAA...)
        std::regex(R"(ssh-(rsa|ed25519|dss)\s+[A-Za-z0-9+/=]{40,})"),
        // OPENAI_API_KEY=value, API_KEY=value, anything *_KEY=value
        std::regex(R"((?:[A-Z][A-Z0-9_]*_)?API_KEY\s*=\s*[^\s,;\"']{4,})",
                   std::regex::icase),
        // x-api-key header value
        std::regex(R"(x-api-key\s*[:=]\s*[^\s,;\"']{4,})",
                   std::regex::icase),
        // Authorization: Basic XXXX
        std::regex(R"(Authorization\s*:\s*Basic\s+[A-Za-z0-9+/=]{8,})",
                   std::regex::icase),
        // private_key "..."
        std::regex(R"(private_key\s*[=:]\s*\"[^\"]{8,}\")",
                   std::regex::icase),
        // -----BEGIN RSA PRIVATE KEY----- ... -----END
        std::regex(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)"),
        // password='...' / password="..." (the core layer only catches `password=` followed by non-space)
        std::regex(R"(password\s*=\s*['"][^'"]{1,}['"])",
                   std::regex::icase),
        // secret='...' / secret="..."
        std::regex(R"(secret\s*=\s*['"][^'"]{1,}['"])",
                   std::regex::icase),
    };
    return table;
}

}  // namespace

std::string strip_credentials(std::string_view input) {
    // First pass: core redactor (Phase 0).
    std::string s = hermes::core::redact::redact_secrets(input);

    // Second pass: MCP-specific overlays.
    for (const auto& rx : mcp_patterns()) {
        s = std::regex_replace(s, rx, kReplacement);
    }

    return s;
}

}  // namespace hermes::approval
