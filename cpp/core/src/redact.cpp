#include "hermes/core/redact.hpp"

#include <array>
#include <regex>
#include <string>
#include <utility>

namespace hermes::core::redact {

namespace {

constexpr const char* kReplacement = "***REDACTED***";

// Compile-once regex table. Each pattern captures the full token so the
// replacement consumes the whole match.
const std::array<std::regex, 7>& patterns() {
    static const std::array<std::regex, 7> table{
        std::regex(R"(sk-[A-Za-z0-9_-]{20,})"),
        std::regex(R"(ghp_[A-Za-z0-9]{20,})"),
        std::regex(R"(Bearer [A-Za-z0-9._-]{20,})"),
        std::regex(R"(token=[A-Za-z0-9._-]{20,})"),
        std::regex(R"(key=[A-Za-z0-9._-]{20,})"),
        std::regex(R"(password=[^\s&]+)"),
        std::regex(R"(secret=[^\s&]+)"),
    };
    return table;
}

}  // namespace

std::string redact_secrets(std::string_view input) {
    std::string current(input);
    for (const auto& rx : patterns()) {
        current = std::regex_replace(current, rx, kReplacement);
    }
    return current;
}

}  // namespace hermes::core::redact
