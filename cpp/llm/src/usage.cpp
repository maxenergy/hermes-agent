// Usage normalization, pricing, and compact formatters.
#include "hermes/llm/usage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace hermes::llm {

using nlohmann::json;

namespace {

int64_t json_int(const json& j, const char* key, int64_t fallback = 0) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto& v = j.at(key);
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return static_cast<int64_t>(v.get<uint64_t>());
    if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
    return fallback;
}

}  // namespace

CanonicalUsage normalize_openai_usage(const json& usage_json) {
    CanonicalUsage u;
    if (!usage_json.is_object()) return u;
    u.input_tokens = json_int(usage_json, "prompt_tokens");
    u.output_tokens = json_int(usage_json, "completion_tokens");
    // OpenAI prompt_tokens_details.cached_tokens (newer API).
    if (usage_json.contains("prompt_tokens_details") &&
        usage_json["prompt_tokens_details"].is_object()) {
        u.cache_read_input_tokens =
            json_int(usage_json["prompt_tokens_details"], "cached_tokens");
    }
    // OpenAI completion_tokens_details.reasoning_tokens.
    if (usage_json.contains("completion_tokens_details") &&
        usage_json["completion_tokens_details"].is_object()) {
        u.reasoning_tokens =
            json_int(usage_json["completion_tokens_details"], "reasoning_tokens");
    }
    return u;
}

CanonicalUsage normalize_anthropic_usage(const json& usage_json) {
    CanonicalUsage u;
    if (!usage_json.is_object()) return u;
    u.input_tokens = json_int(usage_json, "input_tokens");
    u.output_tokens = json_int(usage_json, "output_tokens");
    u.cache_read_input_tokens = json_int(usage_json, "cache_read_input_tokens");
    u.cache_creation_input_tokens =
        json_int(usage_json, "cache_creation_input_tokens");
    return u;
}

double estimate_usage_cost(const CanonicalUsage& u, const PricingTier& p) {
    constexpr double million = 1'000'000.0;
    const double regular_input = static_cast<double>(
        std::max<int64_t>(0, u.input_tokens - u.cache_read_input_tokens -
                                 u.cache_creation_input_tokens));
    double cost = 0.0;
    cost += regular_input * p.input_per_million_usd / million;
    cost += static_cast<double>(u.output_tokens) * p.output_per_million_usd / million;
    cost += static_cast<double>(u.cache_read_input_tokens) *
            p.cache_read_per_million_usd / million;
    cost += static_cast<double>(u.cache_creation_input_tokens) *
            p.cache_write_per_million_usd / million;
    return cost;
}

// ── Hardcoded pricing registry ──────────────────────────────────────────
//
// These prices are realistic (USD per million tokens) but not authoritative
// — Phase 4 will merge live models.dev data on top.  The registry exists so
// that cost math in tests and downstream code has non-zero pricing to work
// with for well-known models.

namespace {

struct Entry {
    const char* key;
    PricingTier tier;
};

constexpr std::array<Entry, 12> kPricing = {{
    // Anthropic Claude 4 family
    {"claude-opus-4-6",   {15.00, 75.00, 1.50, 18.75}},
    {"claude-sonnet-4-6", {3.00, 15.00, 0.30, 3.75}},
    {"claude-haiku-4-5",  {0.80, 4.00, 0.08, 1.00}},
    {"claude-opus",       {15.00, 75.00, 1.50, 18.75}},
    {"claude-sonnet",     {3.00, 15.00, 0.30, 3.75}},
    {"claude-haiku",      {0.80, 4.00, 0.08, 1.00}},
    // OpenAI
    {"gpt-4o",            {2.50, 10.00, 1.25, 0.00}},
    {"gpt-4o-mini",       {0.15, 0.60, 0.075, 0.00}},
    {"gpt-4.1",           {2.00, 8.00, 0.50, 0.00}},
    // DeepSeek
    {"deepseek-chat",     {0.27, 1.10, 0.07, 0.00}},
    // Google
    {"gemini-2.0-flash",  {0.10, 0.40, 0.025, 0.00}},
    {"gemini-2.5-pro",    {1.25, 10.00, 0.3125, 0.00}},
}};

// Case-insensitive "contains" check.
bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

}  // namespace

PricingTier lookup_pricing(std::string_view model) {
    for (const auto& e : kPricing) {
        if (contains_ci(model, e.key)) {
            return e.tier;
        }
    }
    return {};
}

// ── Formatters ──────────────────────────────────────────────────────────

std::string format_token_count_compact(int64_t tokens) {
    if (tokens < 0) tokens = 0;
    char buf[32];
    if (tokens < 1000) {
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(tokens));
        return buf;
    }
    if (tokens < 1'000'000) {
        std::snprintf(buf, sizeof(buf), "%.1fk",
                      static_cast<double>(tokens) / 1000.0);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.1fM",
                  static_cast<double>(tokens) / 1'000'000.0);
    return buf;
}

std::string format_duration_compact(std::chrono::milliseconds d) {
    const int64_t ms = d.count();
    char buf[32];
    if (ms < 1000) {
        std::snprintf(buf, sizeof(buf), "%lldms",
                      static_cast<long long>(ms));
        return buf;
    }
    if (ms < 60'000) {
        std::snprintf(buf, sizeof(buf), "%.1fs",
                      static_cast<double>(ms) / 1000.0);
        return buf;
    }
    const int64_t total_s = ms / 1000;
    const int64_t minutes = total_s / 60;
    const int64_t seconds = total_s % 60;
    std::snprintf(buf, sizeof(buf), "%lldm%llds",
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
    return buf;
}

}  // namespace hermes::llm
