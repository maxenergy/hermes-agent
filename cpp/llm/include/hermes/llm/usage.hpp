// CanonicalUsage normalization + pricing lookup + cost estimation.
// Port of agent/usage_pricing.py (subset required for Phase 3).
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hermes::llm {

struct CanonicalUsage {
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_read_input_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t reasoning_tokens = 0;
};

CanonicalUsage normalize_openai_usage(const nlohmann::json& usage_json);
CanonicalUsage normalize_anthropic_usage(const nlohmann::json& usage_json);

struct PricingTier {
    double input_per_million_usd = 0.0;
    double output_per_million_usd = 0.0;
    double cache_read_per_million_usd = 0.0;
    double cache_write_per_million_usd = 0.0;
};

double estimate_usage_cost(const CanonicalUsage& u, const PricingTier& p);

// Pricing registry: hardcoded well-known models.  Later phases merge live
// models.dev data on top of this.  Returns a zero-cost PricingTier when the
// model is unknown so callers can short-circuit reporting.
PricingTier lookup_pricing(std::string_view model);

// "12345" → "12.3k", "1234567" → "1.2M"
std::string format_token_count_compact(int64_t tokens);
// "450ms", "2.3s", "1m14s"
std::string format_duration_compact(std::chrono::milliseconds d);

}  // namespace hermes::llm
