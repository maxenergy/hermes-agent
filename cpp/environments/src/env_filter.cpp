#include "hermes/environments/env_filter.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace hermes::environments {

namespace {

// Suffix patterns (checked case-insensitively).
constexpr std::array<const char*, 5> kSensitiveSuffixes = {{
    "_API_KEY",
    "_TOKEN",
    "_SECRET",
    "_PASSWORD",
    "_CLIENT_SECRET",
}};

// Exact-match blocklist (case-sensitive).
constexpr std::array<const char*, 11> kBlocklist = {{
    "ANTHROPIC_API_KEY",
    "OPENAI_API_KEY",
    "OPENROUTER_API_KEY",
    "TELEGRAM_BOT_TOKEN",
    "DISCORD_BOT_TOKEN",
    "SLACK_BOT_TOKEN",
    "AWS_SECRET_ACCESS_KEY",
    "GITHUB_TOKEN",
    "HERMES_API_KEY",
    "DATABASE_URL",
    "REDIS_URL",
}};

bool ends_with_ci(std::string_view haystack, std::string_view needle) {
    if (haystack.size() < needle.size()) return false;
    auto start = haystack.size() - needle.size();
    for (std::size_t i = 0; i < needle.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(haystack[start + i])) !=
            std::toupper(static_cast<unsigned char>(needle[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool is_sensitive_var(std::string_view name) {
    // Exact blocklist match.
    for (const auto* blocked : kBlocklist) {
        if (name == blocked) return true;
    }

    // Suffix match (case-insensitive).
    for (const auto* suffix : kSensitiveSuffixes) {
        if (ends_with_ci(name, suffix)) return true;
    }

    return false;
}

std::unordered_map<std::string, std::string> filter_env(
    const std::unordered_map<std::string, std::string>& input) {
    std::unordered_map<std::string, std::string> result;
    for (const auto& [key, value] : input) {
        if (!is_sensitive_var(key)) {
            result.emplace(key, value);
        }
    }
    return result;
}

}  // namespace hermes::environments
