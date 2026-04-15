// C++17 port of `hermes_cli/model_normalize.py`.
//
// The implementation mirrors the Python module verbatim: same vendor
// table, same frozenset memberships, same control flow.  See the
// header for provider classifications.

#include "hermes/cli/model_normalize.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hermes::cli::model_normalize {
namespace {

// Vendor prefix mapping -- maps the first hyphen-delimited token of a
// bare model name to the vendor slug used by aggregator APIs.
const std::unordered_map<std::string, std::string>& vendor_prefixes() {
    static const std::unordered_map<std::string, std::string> table{
        {"claude", "anthropic"},
        {"gpt", "openai"},
        {"o1", "openai"},
        {"o3", "openai"},
        {"o4", "openai"},
        {"gemini", "google"},
        {"gemma", "google"},
        {"deepseek", "deepseek"},
        {"glm", "z-ai"},
        {"kimi", "moonshotai"},
        {"minimax", "minimax"},
        {"grok", "x-ai"},
        {"qwen", "qwen"},
        {"mimo", "xiaomi"},
        {"nemotron", "nvidia"},
        {"llama", "meta-llama"},
        {"step", "stepfun"},
        {"trinity", "arcee-ai"},
    };
    return table;
}

const std::unordered_set<std::string>& aggregator_providers() {
    static const std::unordered_set<std::string> set{
        "openrouter", "nous", "ai-gateway", "kilocode",
    };
    return set;
}

const std::unordered_set<std::string>& dot_to_hyphen_providers() {
    static const std::unordered_set<std::string> set{
        "anthropic", "opencode-zen",
    };
    return set;
}

const std::unordered_set<std::string>& strip_vendor_only_providers() {
    static const std::unordered_set<std::string> set{
        "copilot", "copilot-acp",
    };
    return set;
}

const std::unordered_set<std::string>& authoritative_native_providers() {
    static const std::unordered_set<std::string> set{
        "gemini", "huggingface", "openai-codex",
    };
    return set;
}

const std::unordered_set<std::string>& matching_prefix_strip_providers() {
    static const std::unordered_set<std::string> set{
        "zai", "kimi-coding", "minimax", "minimax-cn",
        "alibaba", "qwen-oauth", "custom",
    };
    return set;
}

// DeepSeek special-case keyword set.
const std::unordered_set<std::string>& deepseek_reasoner_keywords() {
    static const std::unordered_set<std::string> set{
        "reasoner", "r1", "think", "reasoning", "cot",
    };
    return set;
}

const std::unordered_set<std::string>& deepseek_canonical_models() {
    static const std::unordered_set<std::string> set{
        "deepseek-chat", "deepseek-reasoner",
    };
    return set;
}

std::string to_lower_copy(const std::string& value) {
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(const std::string& value) {
    auto is_space = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string{first, last};
}

provider_alias_resolver_t& resolver_slot() {
    static provider_alias_resolver_t slot{};
    return slot;
}

std::string default_resolver(const std::string& raw) {
    return to_lower_copy(trim(raw));
}

std::string resolve_alias(const std::string& raw) {
    const auto& slot = resolver_slot();
    if (slot) {
        return slot(raw);
    }
    return default_resolver(raw);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

void set_provider_alias_resolver(provider_alias_resolver_t resolver) {
    resolver_slot() = std::move(resolver);
}

std::string strip_vendor_prefix(const std::string& model_name) {
    auto slash = model_name.find('/');
    if (slash == std::string::npos) {
        return model_name;
    }
    return model_name.substr(slash + 1);
}

std::string dots_to_hyphens(const std::string& model_name) {
    std::string result{model_name};
    std::replace(result.begin(), result.end(), '.', '-');
    return result;
}

std::optional<std::string> detect_vendor(const std::string& model_name) {
    const std::string name{trim(model_name)};
    if (name.empty()) {
        return std::nullopt;
    }

    // If there's already a vendor/ prefix, extract it.
    auto slash = name.find('/');
    if (slash != std::string::npos) {
        std::string prefix{name.substr(0, slash)};
        if (prefix.empty()) {
            return std::nullopt;
        }
        return to_lower_copy(prefix);
    }

    const std::string name_lower{to_lower_copy(name)};

    // Try first hyphen-delimited token (exact match).
    std::string first_token{name_lower};
    auto hyphen = name_lower.find('-');
    if (hyphen != std::string::npos) {
        first_token = name_lower.substr(0, hyphen);
    }
    const auto& table = vendor_prefixes();
    auto it = table.find(first_token);
    if (it != table.end()) {
        return it->second;
    }

    // Handle patterns where the first token includes version digits,
    // e.g. "qwen3.5-plus".
    for (const auto& [prefix, vendor] : table) {
        if (starts_with(name_lower, prefix)) {
            return vendor;
        }
    }

    return std::nullopt;
}

std::string prepend_vendor(const std::string& model_name) {
    if (model_name.find('/') != std::string::npos) {
        return model_name;
    }
    auto vendor = detect_vendor(model_name);
    if (vendor.has_value()) {
        return *vendor + "/" + model_name;
    }
    return model_name;
}

std::string strip_matching_provider_prefix(const std::string& model_name,
                                           const std::string& target_provider) {
    auto slash = model_name.find('/');
    if (slash == std::string::npos) {
        return model_name;
    }

    std::string prefix{model_name.substr(0, slash)};
    std::string remainder{model_name.substr(slash + 1)};
    if (trim(prefix).empty() || trim(remainder).empty()) {
        return model_name;
    }

    std::string normalized_prefix{resolve_alias(prefix)};
    std::string normalized_target{resolve_alias(target_provider)};
    if (!normalized_prefix.empty() && normalized_prefix == normalized_target) {
        return trim(remainder);
    }
    return model_name;
}

std::string normalize_for_deepseek(const std::string& model_name) {
    std::string bare{to_lower_copy(strip_vendor_prefix(model_name))};

    if (deepseek_canonical_models().count(bare) > 0) {
        return bare;
    }

    for (const auto& keyword : deepseek_reasoner_keywords()) {
        if (contains(bare, keyword)) {
            return std::string{"deepseek-reasoner"};
        }
    }

    return std::string{"deepseek-chat"};
}

std::string normalize_model_for_provider(const std::string& model_input,
                                         const std::string& target_provider) {
    std::string name{trim(model_input)};
    if (name.empty()) {
        return name;
    }

    std::string provider{resolve_alias(target_provider)};

    if (aggregator_providers().count(provider) > 0) {
        return prepend_vendor(name);
    }

    if (dot_to_hyphen_providers().count(provider) > 0) {
        std::string bare{strip_matching_provider_prefix(name, provider)};
        if (bare.find('/') != std::string::npos) {
            return bare;
        }
        return dots_to_hyphens(bare);
    }

    if (strip_vendor_only_providers().count(provider) > 0) {
        return strip_matching_provider_prefix(name, provider);
    }

    if (provider == "deepseek") {
        std::string bare{strip_matching_provider_prefix(name, provider)};
        if (bare.find('/') != std::string::npos) {
            return bare;
        }
        return normalize_for_deepseek(bare);
    }

    if (matching_prefix_strip_providers().count(provider) > 0) {
        return strip_matching_provider_prefix(name, provider);
    }

    if (authoritative_native_providers().count(provider) > 0) {
        return name;
    }

    return name;
}

bool is_aggregator_provider(const std::string& canonical_provider) {
    return aggregator_providers().count(canonical_provider) > 0;
}

bool is_dot_to_hyphen_provider(const std::string& canonical_provider) {
    return dot_to_hyphen_providers().count(canonical_provider) > 0;
}

bool is_strip_vendor_only_provider(const std::string& canonical_provider) {
    return strip_vendor_only_providers().count(canonical_provider) > 0;
}

bool is_authoritative_native_provider(const std::string& canonical_provider) {
    return authoritative_native_providers().count(canonical_provider) > 0;
}

bool is_matching_prefix_strip_provider(const std::string& canonical_provider) {
    return matching_prefix_strip_providers().count(canonical_provider) > 0;
}

}  // namespace hermes::cli::model_normalize
