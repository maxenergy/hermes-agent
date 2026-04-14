#include "hermes/agent/aux_provider_alias.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace hermes::agent::aux {

namespace {

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

const std::unordered_map<std::string, std::string>& aliases() {
    static const std::unordered_map<std::string, std::string> m = {
        {"google", "gemini"},
        {"google-gemini", "gemini"},
        {"google-ai-studio", "gemini"},
        {"glm", "zai"},
        {"z-ai", "zai"},
        {"z.ai", "zai"},
        {"zhipu", "zai"},
        {"kimi", "kimi-coding"},
        {"moonshot", "kimi-coding"},
        {"minimax-china", "minimax-cn"},
        {"minimax_cn", "minimax-cn"},
        {"claude", "anthropic"},
        {"claude-code", "anthropic"},
    };
    return m;
}

const std::unordered_map<std::string, std::string>& defaults() {
    static const std::unordered_map<std::string, std::string> m = {
        {"anthropic", "claude-haiku-4"},
        {"openai", "gpt-4o-mini"},
        {"openrouter", "anthropic/claude-3-haiku"},
        {"gemini", "gemini-2.0-flash"},
        {"zai", "glm-4.5-flash"},
        {"kimi-coding", "kimi-k2-turbo"},
        {"minimax", "MiniMax-Text-01"},
        {"minimax-cn", "MiniMax-Text-01"},
        {"openai-codex", "gpt-5-codex"},
    };
    return m;
}

}  // namespace

std::optional<std::string> alias_target(const std::string& provider) {
    const std::string key = lower_copy(trim(provider));
    const auto& m = aliases();
    auto it = m.find(key);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

std::string normalize_provider(const std::string& provider, bool for_vision) {
    std::string normalized = lower_copy(trim(provider));
    if (normalized.empty()) return "auto";

    if (normalized.rfind("custom:", 0) == 0) {
        std::string suffix = trim(normalized.substr(7));
        if (suffix.empty()) return "custom";
        if (for_vision) return "custom";
        normalized = suffix;
    }
    if (normalized == "codex") return "openai-codex";
    if (normalized == "main") return "main";  // caller substitutes

    const auto& m = aliases();
    auto it = m.find(normalized);
    if (it != m.end()) return it->second;
    return normalized;
}

std::string default_model_for(const std::string& provider) {
    const std::string key = lower_copy(trim(provider));
    const auto& m = defaults();
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

}  // namespace hermes::agent::aux
