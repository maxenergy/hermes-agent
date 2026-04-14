// C++17 port of hermes_cli/auth_commands.py — interactive auth pickers,
// pool display formatters, and argparse-style command structs.
#include "hermes/cli/auth_commands.hpp"

#include "hermes/cli/providers_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace hermes::cli::auth_commands {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

const char* const AUTH_TYPE_API_KEY   = "api_key";
const char* const AUTH_TYPE_OAUTH     = "oauth";
const char* const SOURCE_MANUAL       = "manual";
const char* const STATUS_EXHAUSTED    = "exhausted";
const char* const CUSTOM_POOL_PREFIX  = "custom:";

const char* const STRATEGY_FILL_FIRST = "fill_first";
const char* const STRATEGY_ROUND_ROBIN = "round_robin";
const char* const STRATEGY_LEAST_USED  = "least_used";
const char* const STRATEGY_RANDOM      = "random";

const std::vector<std::string>& oauth_capable_providers() {
    static const std::vector<std::string> v = {
        "anthropic", "nous", "openai-codex", "qwen-oauth",
    };
    return v;
}

std::string normalize_custom_pool_name(const std::string& display_name) {
    std::string s = to_lower(trim(display_name));
    for (auto& c : s) {
        if (c == ' ') c = '-';
    }
    return s;
}

std::string resolve_custom_provider_input(const std::string& raw,
                                          const nlohmann::json& custom_providers) {
    std::string normalized = to_lower(trim(raw));
    for (auto& c : normalized) {
        if (c == ' ') c = '-';
    }
    if (normalized.empty()) return "";
    if (normalized.rfind(CUSTOM_POOL_PREFIX, 0) == 0) return normalized;
    if (!custom_providers.is_array()) return "";
    for (const auto& entry : custom_providers) {
        if (!entry.is_object()) continue;
        auto it = entry.find("name");
        if (it == entry.end() || !it->is_string()) continue;
        std::string name = it->get<std::string>();
        if (normalize_custom_pool_name(name) == normalized) {
            return std::string(CUSTOM_POOL_PREFIX) + normalize_custom_pool_name(name);
        }
    }
    return "";
}

std::string normalize_provider(const std::string& raw,
                               const nlohmann::json& custom_providers) {
    std::string normalized = to_lower(trim(raw));
    if (normalized == "or" || normalized == "open-router") return "openrouter";
    std::string custom = resolve_custom_provider_input(normalized, custom_providers);
    if (!custom.empty()) return custom;
    return normalized;
}

std::string oauth_default_label(const std::string& provider, std::size_t count) {
    std::ostringstream oss;
    oss << provider << "-oauth-" << count;
    return oss.str();
}

std::string api_key_default_label(std::size_t count) {
    std::ostringstream oss;
    oss << "api-key-" << count;
    return oss.str();
}

std::string display_source(const std::string& source) {
    const std::string prefix = "manual:";
    if (source.rfind(prefix, 0) == 0) return source.substr(prefix.size());
    return source;
}

std::string provider_base_url(const std::string& provider,
                              const nlohmann::json& custom_providers) {
    if (provider == "openrouter") return "https://openrouter.ai/api/v1";
    if (provider.rfind(CUSTOM_POOL_PREFIX, 0) == 0) {
        if (!custom_providers.is_array()) return "";
        std::string slug = provider.substr(std::string(CUSTOM_POOL_PREFIX).size());
        for (const auto& entry : custom_providers) {
            if (!entry.is_object()) continue;
            auto name_it = entry.find("name");
            if (name_it == entry.end() || !name_it->is_string()) continue;
            if (normalize_custom_pool_name(name_it->get<std::string>()) != slug) continue;
            for (const auto* k : {"base_url", "url", "api"}) {
                auto v = entry.find(k);
                if (v != entry.end() && v->is_string()) {
                    return trim(v->get<std::string>());
                }
            }
            return "";
        }
        return "";
    }
    auto pdef = providers_cmd::get_provider(provider);
    if (pdef) return pdef->base_url;
    return "";
}

std::string format_exhausted_status(const PooledEntry& entry, double now_epoch) {
    if (entry.last_status != STATUS_EXHAUSTED) return "";
    std::string reason_text;
    if (!entry.last_error_reason.empty()) {
        reason_text = " " + entry.last_error_reason;
    }
    std::string code;
    if (!entry.last_error_code.empty()) {
        code = " (" + entry.last_error_code + ")";
    }
    if (!entry.exhausted_until.has_value()) {
        return " exhausted" + reason_text + code;
    }
    double remaining_d = *entry.exhausted_until - now_epoch;
    long long remaining = static_cast<long long>(std::ceil(remaining_d));
    if (remaining <= 0) {
        return " exhausted" + reason_text + code + " (ready to retry)";
    }
    long long seconds = remaining % 60;
    long long minutes = (remaining / 60) % 60;
    long long hours = (remaining / 3600) % 24;
    long long days = remaining / 86400;
    std::ostringstream wait;
    if (days > 0) {
        wait << days << "d " << hours << "h";
    } else if (hours > 0) {
        wait << hours << "h " << minutes << "m";
    } else if (minutes > 0) {
        wait << minutes << "m " << seconds << "s";
    } else {
        wait << seconds << "s";
    }
    return " exhausted" + reason_text + code + " (" + wait.str() + " left)";
}

std::string render_list_table(const std::string& provider,
                              const std::vector<PooledEntry>& entries,
                              const std::optional<std::string>& current_id,
                              double now_epoch) {
    std::ostringstream oss;
    if (entries.empty()) return "";
    oss << provider << " (" << entries.size() << " credentials):\n";
    std::size_t idx = 1;
    for (const auto& e : entries) {
        std::string marker = "  ";
        if (current_id && e.id == *current_id) marker = "← ";
        std::string status = format_exhausted_status(e, now_epoch);
        std::string source = display_source(e.source);
        std::ostringstream line;
        line << "  #" << idx << "  ";
        // pad label to 20 chars
        std::string label = e.label;
        if (label.size() < 20) label.append(20 - label.size(), ' ');
        line << label << ' ';
        // pad auth_type to 7
        std::string at = e.auth_type;
        if (at.size() < 7) at.append(7 - at.size(), ' ');
        line << at << ' ' << source << status << ' ' << marker;
        // Mirror Python's rstrip on trailing whitespace.
        std::string s = line.str();
        auto end = s.find_last_not_of(' ');
        if (end != std::string::npos) s.resize(end + 1);
        oss << s << '\n';
        ++idx;
    }
    oss << '\n';
    return oss.str();
}

std::string resolve_auth_type(const std::string& requested,
                              const std::string& canonical_provider) {
    std::string r = to_lower(trim(requested));
    if (r == "api-key") r = AUTH_TYPE_API_KEY;
    if (!r.empty()) return r;
    if (canonical_provider.rfind(CUSTOM_POOL_PREFIX, 0) == 0) return AUTH_TYPE_API_KEY;
    for (const auto& p : oauth_capable_providers()) {
        if (p == canonical_provider) return AUTH_TYPE_OAUTH;
    }
    return AUTH_TYPE_API_KEY;
}

std::string validate_provider_choice(const std::string& canonical_provider) {
    if (canonical_provider.empty()) return "Empty provider name.";
    if (canonical_provider == "openrouter") return "";
    if (canonical_provider.rfind(CUSTOM_POOL_PREFIX, 0) == 0) return "";
    auto pdef = providers_cmd::get_provider(canonical_provider);
    if (!pdef) return "Unknown provider: " + canonical_provider;
    return "";
}

std::string env_var_guidance(const std::string& canonical_provider) {
    auto pdef = providers_cmd::get_provider(canonical_provider);
    if (!pdef) return "";
    if (pdef->api_key_env_vars.empty()) return "";
    std::ostringstream oss;
    oss << "Set one of: ";
    for (std::size_t i = 0; i < pdef->api_key_env_vars.size(); ++i) {
        if (i) oss << ", ";
        oss << pdef->api_key_env_vars[i];
    }
    return oss.str();
}

std::string render_provider_picker_hint(
    const std::vector<std::string>& known_providers,
    const std::vector<std::string>& custom_display_names) {
    std::ostringstream oss;
    oss << "\nKnown providers: ";
    for (std::size_t i = 0; i < known_providers.size(); ++i) {
        if (i) oss << ", ";
        oss << known_providers[i];
    }
    if (!custom_display_names.empty()) {
        oss << "\nCustom endpoints: ";
        for (std::size_t i = 0; i < custom_display_names.size(); ++i) {
            if (i) oss << ", ";
            oss << custom_display_names[i];
        }
    }
    return oss.str();
}

const std::vector<StrategyChoice>& strategy_catalog() {
    static const std::vector<StrategyChoice> v = {
        {STRATEGY_FILL_FIRST,  "Use first key until exhausted, then next"},
        {STRATEGY_ROUND_ROBIN, "Cycle through keys evenly"},
        {STRATEGY_LEAST_USED,  "Always pick the least-used key"},
        {STRATEGY_RANDOM,      "Random selection"},
    };
    return v;
}

bool apply_strategy_choice(nlohmann::json& config,
                           const std::string& provider,
                           std::size_t choice_index) {
    const auto& cat = strategy_catalog();
    if (choice_index >= cat.size()) return false;
    if (!config.is_object()) config = nlohmann::json::object();
    auto& map = config["credential_pool_strategies"];
    if (!map.is_object()) map = nlohmann::json::object();
    map[provider] = cat[choice_index].name;
    return true;
}

std::string read_strategy(const nlohmann::json& config,
                          const std::string& provider) {
    if (config.is_object()) {
        auto it = config.find("credential_pool_strategies");
        if (it != config.end() && it->is_object()) {
            auto v = it->find(provider);
            if (v != it->end() && v->is_string()) return v->get<std::string>();
        }
    }
    return STRATEGY_FILL_FIRST;
}

}  // namespace hermes::cli::auth_commands
