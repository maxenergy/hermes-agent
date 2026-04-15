// Pure-logic helpers ported from `hermes_cli/runtime_provider.py`.
#include "hermes/cli/runtime_provider_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_map>

namespace hermes::cli::runtime_provider_helpers {

namespace {

std::string trim(std::string s) {
    size_t i {0};
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string replace_spaces_with_dash(std::string s) {
    for (char& c : s) {
        if (c == ' ') {
            c = '-';
        }
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size()
        && std::equal(p.begin(), p.end(), s.begin());
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size()
        && std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

}  // namespace

// ---------------------------------------------------------------------------

std::string normalize_custom_provider_name(const std::string& value) {
    return replace_spaces_with_dash(to_lower(trim(value)));
}

std::optional<std::string> detect_api_mode_for_url(
    const std::string& base_url) {
    std::string normalised {to_lower(trim(base_url))};
    while (!normalised.empty() && normalised.back() == '/') {
        normalised.pop_back();
    }
    const bool has_openai {
        normalised.find("api.openai.com") != std::string::npos};
    const bool has_openrouter {
        normalised.find("openrouter") != std::string::npos};
    if (has_openai && !has_openrouter) {
        return std::string {"codex_responses"};
    }
    return std::nullopt;
}

std::optional<std::string> parse_api_mode(const std::string& raw) {
    const std::string normalised {to_lower(trim(raw))};
    if (normalised == "chat_completions"
        || normalised == "codex_responses"
        || normalised == "anthropic_messages") {
        return normalised;
    }
    return std::nullopt;
}

bool is_local_base_url(const std::string& base_url) {
    return base_url.find("localhost") != std::string::npos
        || base_url.find("127.0.0.1") != std::string::npos;
}

bool provider_supports_explicit_api_mode(
    const std::string& provider,
    const std::string& configured_provider) {
    const std::string normalised_provider {to_lower(trim(provider))};
    const std::string normalised_configured {to_lower(trim(configured_provider))};
    if (normalised_configured.empty()) {
        return true;
    }
    if (normalised_provider == "custom") {
        return normalised_configured == "custom"
            || starts_with(normalised_configured, "custom:");
    }
    return normalised_configured == normalised_provider;
}

// ---------------------------------------------------------------------------

std::string default_base_url_for_provider(const std::string& provider) {
    static const std::unordered_map<std::string, std::string> defaults {
        {"openai-codex", kDefaultCodexBaseUrl},
        {"qwen-oauth", kDefaultQwenBaseUrl},
        {"anthropic", kDefaultAnthropicBaseUrl},
        {"openrouter", kDefaultOpenRouterBaseUrl},
    };
    const auto it {defaults.find(provider)};
    if (it != defaults.end()) {
        return it->second;
    }
    return {};
}

std::string default_api_mode_for_provider(const std::string& provider) {
    if (provider == "openai-codex") {
        return "codex_responses";
    }
    if (provider == "anthropic") {
        return "anthropic_messages";
    }
    return "chat_completions";
}

// ---------------------------------------------------------------------------

std::string strip_trailing_slash(const std::string& url) {
    if (!url.empty() && url.back() == '/') {
        return url.substr(0, url.size() - 1);
    }
    return url;
}

std::string strip_v1_suffix(const std::string& url) {
    static const std::regex re {R"(/v1/?$)"};
    return std::regex_replace(url, re, "");
}

bool url_hints_anthropic_mode(const std::string& base_url) {
    return ends_with(strip_trailing_slash(base_url), "/anthropic");
}

// ---------------------------------------------------------------------------

std::string resolve_requested_provider(
    const std::string& requested,
    const std::string& cfg_provider,
    const std::string& env_provider) {
    const std::string r {trim(requested)};
    if (!r.empty()) {
        return to_lower(r);
    }
    const std::string c {trim(cfg_provider)};
    if (!c.empty()) {
        return to_lower(c);
    }
    const std::string e {trim(env_provider)};
    if (!e.empty()) {
        return to_lower(e);
    }
    return "auto";
}

// ---------------------------------------------------------------------------

std::string format_runtime_provider_error(const ErrorContext& ctx) {
    // Strip ANSI CSI escapes.
    static const std::regex ansi_re {R"(\x1b\[[0-9;]*[A-Za-z])"};
    const std::string clean_msg {std::regex_replace(ctx.message, ansi_re, "")};

    if (ctx.error_class == "AuthError") {
        const std::string label {
            ctx.provider.empty() ? std::string {"runtime"} : ctx.provider};
        return "Authentication failed for " + label + ": " + clean_msg;
    }
    if (ctx.error_class == "ValueError"
        || ctx.error_class == "invalid_argument") {
        return "Invalid runtime configuration: " + clean_msg;
    }
    if (ctx.error_class == "FileNotFoundError") {
        return "Required file missing: " + clean_msg;
    }
    if (ctx.provider.empty()) {
        return clean_msg;
    }
    return "Failed to resolve runtime provider '" + ctx.provider
         + "': " + clean_msg;
}

// ---------------------------------------------------------------------------

ResolvedRuntime resolve_runtime_from_pool_entry(
    const std::string& provider,
    const PoolEntry& entry,
    const std::string& requested_provider,
    const ModelConfig& model_cfg) {
    ResolvedRuntime out {};
    out.provider = provider;
    out.requested_provider = requested_provider;

    std::string base_url {
        !entry.runtime_base_url.empty() ? entry.runtime_base_url
                                        : entry.base_url};
    base_url = strip_trailing_slash(base_url);

    std::string api_key {
        !entry.runtime_api_key.empty() ? entry.runtime_api_key
                                       : entry.access_token};
    out.api_key = api_key;
    out.source = entry.source.empty() ? std::string {"pool"} : entry.source;

    std::string api_mode {"chat_completions"};

    if (provider == "openai-codex") {
        api_mode = "codex_responses";
        if (base_url.empty()) {
            base_url = kDefaultCodexBaseUrl;
        }
    } else if (provider == "qwen-oauth") {
        api_mode = "chat_completions";
        if (base_url.empty()) {
            base_url = kDefaultQwenBaseUrl;
        }
    } else if (provider == "anthropic") {
        api_mode = "anthropic_messages";
        const std::string cfg_provider {to_lower(trim(model_cfg.provider))};
        std::string cfg_base_url {};
        if (cfg_provider == "anthropic") {
            cfg_base_url = strip_trailing_slash(trim(model_cfg.base_url));
        }
        if (!cfg_base_url.empty()) {
            base_url = cfg_base_url;
        } else if (base_url.empty()) {
            base_url = kDefaultAnthropicBaseUrl;
        }
    } else if (provider == "openrouter") {
        if (base_url.empty()) {
            base_url = kDefaultOpenRouterBaseUrl;
        }
    } else if (provider == "nous") {
        api_mode = "chat_completions";
    } else if (provider == "copilot") {
        const auto configured {parse_api_mode(model_cfg.api_mode)};
        if (configured
            && provider_supports_explicit_api_mode(
                   "copilot", model_cfg.provider)) {
            api_mode = *configured;
        }
    } else {
        // Generic branch: honour explicit api_mode when the configured
        // provider matches, and check for /anthropic suffix.
        const auto configured {parse_api_mode(model_cfg.api_mode)};
        if (configured
            && provider_supports_explicit_api_mode(
                   provider, model_cfg.provider)) {
            api_mode = *configured;
        } else if (url_hints_anthropic_mode(base_url)) {
            api_mode = "anthropic_messages";
        }
    }

    // OpenCode + anthropic_messages: drop /v1 suffix so SDK composes
    // /v1/messages correctly.
    if (api_mode == "anthropic_messages"
        && (provider == "opencode-zen" || provider == "opencode-go")) {
        base_url = strip_v1_suffix(base_url);
    }

    out.api_mode = api_mode;
    out.base_url = base_url;
    return out;
}

// ---------------------------------------------------------------------------

std::string extract_custom_provider_name(const std::string& label) {
    const std::string trimmed {to_lower(trim(label))};
    const std::string prefix {"custom:"};
    if (starts_with(trimmed, prefix)) {
        return trimmed.substr(prefix.size());
    }
    return {};
}

bool is_pool_backed_provider(const std::string& provider) {
    static const std::vector<std::string> pool_backed {
        "openai-codex", "qwen-oauth", "anthropic", "openrouter",
        "nous", "copilot", "opencode-zen", "opencode-go",
    };
    return std::find(pool_backed.begin(), pool_backed.end(), provider)
        != pool_backed.end();
}

}  // namespace hermes::cli::runtime_provider_helpers
