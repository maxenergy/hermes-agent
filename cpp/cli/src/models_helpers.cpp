// Helpers layered on top of models_cmd — port of hermes_cli/models.py.
#include "hermes/cli/models_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace hermes::cli::models_helpers {

namespace {

std::string strip(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool try_parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

double pricing_field_or_default(const nlohmann::json& pricing,
                                const std::string& key, double def) {
    auto it = pricing.find(key);
    if (it == pricing.end()) return def;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        double v = 0.0;
        return try_parse_double(it->get<std::string>(), v) ? v : def;
    }
    return def;
}

}  // namespace

// -------------------------------------------------------------------------
// OpenRouter snapshot.
// -------------------------------------------------------------------------

const std::vector<std::pair<std::string, std::string>>& openrouter_snapshot() {
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"anthropic/claude-opus-4.6", "recommended"},
        {"anthropic/claude-sonnet-4.6", ""},
        {"qwen/qwen3.6-plus", ""},
        {"anthropic/claude-sonnet-4.5", ""},
        {"anthropic/claude-haiku-4.5", ""},
        {"openai/gpt-5.4", ""},
        {"openai/gpt-5.4-mini", ""},
        {"xiaomi/mimo-v2-pro", ""},
        {"openai/gpt-5.3-codex", ""},
        {"google/gemini-3-pro-image-preview", ""},
        {"google/gemini-3-flash-preview", ""},
        {"google/gemini-3.1-pro-preview", ""},
        {"google/gemini-3.1-flash-lite-preview", ""},
        {"qwen/qwen3.5-plus-02-15", ""},
        {"qwen/qwen3.5-35b-a3b", ""},
        {"stepfun/step-3.5-flash", ""},
        {"minimax/minimax-m2.7", ""},
        {"minimax/minimax-m2.5", ""},
        {"z-ai/glm-5.1", ""},
        {"z-ai/glm-5-turbo", ""},
        {"moonshotai/kimi-k2.5", ""},
        {"x-ai/grok-4.20", ""},
        {"nvidia/nemotron-3-super-120b-a12b", ""},
        {"nvidia/nemotron-3-super-120b-a12b:free", "free"},
        {"arcee-ai/trinity-large-preview:free", "free"},
        {"arcee-ai/trinity-large-thinking", ""},
        {"openai/gpt-5.4-pro", ""},
        {"openai/gpt-5.4-nano", ""},
    };
    return v;
}

// -------------------------------------------------------------------------
// Vendor-prefix.
// -------------------------------------------------------------------------

std::string strip_vendor_prefix(const std::string& model_id) {
    auto raw = to_lower(strip(model_id));
    auto slash = raw.find('/');
    if (slash != std::string::npos) {
        raw = raw.substr(slash + 1);
    }
    return raw;
}

// -------------------------------------------------------------------------
// Fast-mode classification.
// -------------------------------------------------------------------------

const std::unordered_set<std::string>& priority_processing_models() {
    static const std::unordered_set<std::string> v = {
        "gpt-5.4", "gpt-5.4-mini", "gpt-5.2", "gpt-5.1", "gpt-5", "gpt-5-mini",
        "gpt-4.1", "gpt-4.1-mini", "gpt-4.1-nano", "gpt-4o", "gpt-4o-mini",
        "o3", "o4-mini",
    };
    return v;
}

const std::unordered_set<std::string>& anthropic_fast_mode_models() {
    static const std::unordered_set<std::string> v = {
        "claude-opus-4-6",
        "claude-opus-4.6",
    };
    return v;
}

bool model_supports_fast_mode(const std::string& model_id) {
    auto raw = strip_vendor_prefix(model_id);
    if (priority_processing_models().count(raw)) return true;
    auto base = raw.substr(0, raw.find(':'));
    return anthropic_fast_mode_models().count(base) > 0;
}

bool is_anthropic_fast_model(const std::string& model_id) {
    auto raw = strip_vendor_prefix(model_id);
    auto base = raw.substr(0, raw.find(':'));
    return anthropic_fast_mode_models().count(base) > 0;
}

std::optional<nlohmann::json> resolve_fast_mode_overrides(
    const std::string& model_id) {
    if (!model_supports_fast_mode(model_id)) return std::nullopt;
    nlohmann::json out = nlohmann::json::object();
    if (is_anthropic_fast_model(model_id)) {
        out["speed"] = "fast";
    } else {
        out["service_tier"] = "priority";
    }
    return out;
}

// -------------------------------------------------------------------------
// Free-model detection.
// -------------------------------------------------------------------------

bool openrouter_model_is_free(const nlohmann::json& pricing) {
    if (!pricing.is_object()) return false;
    const double prompt = pricing_field_or_default(pricing, "prompt", -1.0);
    const double completion = pricing_field_or_default(pricing, "completion", -1.0);
    if (prompt < 0 || completion < 0) {
        // Fields missing — Python default is "0" for the reader, but
        // if both are absent we return false (not a usable signal).
        // Match Python: defaults to "0" so missing is treated as zero
        // → returns true.  Recompute with zero defaults.
        const double p = pricing_field_or_default(pricing, "prompt", 0.0);
        const double c = pricing_field_or_default(pricing, "completion", 0.0);
        return p == 0.0 && c == 0.0;
    }
    return prompt == 0.0 && completion == 0.0;
}

bool is_model_free(const std::string& model_id,
                   const nlohmann::json& pricing_map) {
    if (!pricing_map.is_object()) return false;
    auto it = pricing_map.find(model_id);
    if (it == pricing_map.end() || !it->is_object()) return false;
    // Python uses "1" defaults so missing → 1.0 → not zero → false.
    const double prompt = pricing_field_or_default(*it, "prompt", 1.0);
    const double completion = pricing_field_or_default(*it, "completion", 1.0);
    return prompt == 0.0 && completion == 0.0;
}

// -------------------------------------------------------------------------
// Nous tier helpers.
// -------------------------------------------------------------------------

const std::unordered_set<std::string>& nous_allowed_free_models() {
    static const std::unordered_set<std::string> v = {
        "xiaomi/mimo-v2-pro",
        "xiaomi/mimo-v2-omni",
    };
    return v;
}

std::vector<std::string> filter_nous_free_models(
    const std::vector<std::string>& model_ids,
    const nlohmann::json& pricing_map) {
    if (!pricing_map.is_object() || pricing_map.empty()) return model_ids;
    const auto& allow = nous_allowed_free_models();
    std::vector<std::string> out;
    out.reserve(model_ids.size());
    for (const auto& mid : model_ids) {
        const bool free = is_model_free(mid, pricing_map);
        const bool allowed = allow.count(mid) > 0;
        if (allowed) {
            if (free) out.push_back(mid);
        } else {
            if (!free) out.push_back(mid);
        }
    }
    return out;
}

bool is_nous_free_tier(const nlohmann::json& account_info) {
    if (!account_info.is_object()) return false;
    auto sub = account_info.find("subscription");
    if (sub == account_info.end() || !sub->is_object()) return false;
    auto charge = sub->find("monthly_charge");
    if (charge == sub->end()) return false;
    if (charge->is_null()) return false;
    if (charge->is_number()) return charge->get<double>() == 0.0;
    if (charge->is_string()) {
        double v = 0.0;
        if (!try_parse_double(charge->get<std::string>(), v)) return false;
        return v == 0.0;
    }
    return false;
}

std::pair<std::vector<std::string>, std::vector<std::string>>
partition_nous_models_by_tier(const std::vector<std::string>& model_ids,
                              const nlohmann::json& pricing_map,
                              bool free_tier) {
    if (!free_tier) return {model_ids, {}};
    if (!pricing_map.is_object() || pricing_map.empty()) return {model_ids, {}};
    std::vector<std::string> sel, unavail;
    for (const auto& mid : model_ids) {
        if (is_model_free(mid, pricing_map)) sel.push_back(mid);
        else unavail.push_back(mid);
    }
    return {sel, unavail};
}

// -------------------------------------------------------------------------
// Pricing formatter.
// -------------------------------------------------------------------------

std::string format_price_per_mtok(const std::string& per_token_str) {
    double val = 0.0;
    if (!try_parse_double(per_token_str, val)) return "?";
    if (val == 0.0) return "free";
    const double per_m = val * 1'000'000.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "$%.2f", per_m);
    return buf;
}

// -------------------------------------------------------------------------
// JSON helpers.
// -------------------------------------------------------------------------

std::vector<nlohmann::json> payload_items(const nlohmann::json& payload) {
    std::vector<nlohmann::json> out;
    if (payload.is_array()) {
        for (const auto& it : payload) {
            if (it.is_object()) out.push_back(it);
        }
        return out;
    }
    if (payload.is_object()) {
        auto data = payload.find("data");
        if (data != payload.end() && data->is_array()) {
            for (const auto& it : *data) {
                if (it.is_object()) out.push_back(it);
            }
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// OpenRouter slug lookup.
// -------------------------------------------------------------------------

std::optional<std::string> find_openrouter_slug(
    const std::string& model_name,
    const std::vector<std::string>& catalog_in) {
    auto trimmed = strip(model_name);
    if (trimmed.empty()) return std::nullopt;
    const auto needle = to_lower(trimmed);

    std::vector<std::string> catalog;
    if (catalog_in.empty()) {
        for (const auto& kv : openrouter_snapshot()) catalog.push_back(kv.first);
    } else {
        catalog = catalog_in;
    }

    // Exact (case-insensitive) match.
    for (const auto& mid : catalog) {
        if (to_lower(mid) == needle) return mid;
    }
    // Match the part after `/`.
    for (const auto& mid : catalog) {
        auto slash = mid.find('/');
        if (slash == std::string::npos) continue;
        if (to_lower(mid.substr(slash + 1)) == needle) return mid;
    }
    return std::nullopt;
}

}  // namespace hermes::cli::models_helpers
