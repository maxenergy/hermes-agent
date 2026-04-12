#include "hermes/tools/env_passthrough.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

extern char** environ;

namespace hermes::tools {

namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

constexpr std::array<const char*, 24> kKnownProviders = {
    "OPENAI", "ANTHROPIC", "OPENROUTER", "MISTRAL", "GROQ",
    "TOGETHER", "FIREWORKS", "DEEPSEEK", "DEEPINFRA", "PERPLEXITY",
    "ELEVENLABS", "GEMINI", "GOOGLE", "AZURE", "AWS",
    "XAI", "CEREBRAS", "NOUS", "EXA", "FIRECRAWL",
    "MODAL", "DAYTONA", "TWILIO", "SLACK",
};

}  // namespace

bool is_sensitive_env_var(std::string_view name) {
    if (name.empty()) return false;
    const std::string up = upper(name);

    if (starts_with(up, "HERMES_")) return true;
    if (ends_with(up, "_API_KEY")) return true;
    if (ends_with(up, "_TOKEN")) return true;
    if (ends_with(up, "_SECRET")) return true;
    if (ends_with(up, "_PASSWORD")) return true;
    if (ends_with(up, "_PASSWD")) return true;
    if (ends_with(up, "_KEY")) {
        // Catch "FOO_KEY" but allow PUBLIC_KEY through? Be conservative —
        // anything called *_KEY in the environment is probably credential
        // material.
        return true;
    }

    for (const char* prefix : kKnownProviders) {
        const std::size_t plen = std::strlen(prefix);
        if (up.size() >= plen &&
            up.compare(0, plen, prefix) == 0 &&
            (up.size() == plen || up[plen] == '_')) {
            return true;
        }
    }

    return false;
}

std::unordered_map<std::string, std::string> safe_env_subset() {
    std::unordered_map<std::string, std::string> out;
    if (environ == nullptr) return out;
    for (char** ep = environ; *ep != nullptr; ++ep) {
        std::string entry(*ep);
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string name = entry.substr(0, eq);
        std::string value = entry.substr(eq + 1);
        if (is_sensitive_env_var(name)) continue;
        out.emplace(std::move(name), std::move(value));
    }
    return out;
}

}  // namespace hermes::tools
