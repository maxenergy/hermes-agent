// Pure cost-estimation types + math from agent/usage_pricing.py.
//
// The full Python module fetches pricing from provider APIs (OpenRouter
// models endpoint, Anthropic docs snapshot, custom OpenAI-compatible
// /models). This C++ port ports the pure data types and the billing-
// route resolver + cost math; fetching is deferred to the adapter layer.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hermes::agent::pricing {

// CostStatus / CostSource — string constants mirroring Python's Literal
// types. Kept as plain strings so downstream tooling (JSON logging,
// debug dumps) can round-trip without a bespoke enum → string mapping.
namespace status {
constexpr const char* kActual = "actual";
constexpr const char* kEstimated = "estimated";
constexpr const char* kIncluded = "included";
constexpr const char* kUnknown = "unknown";
}  // namespace status

namespace source {
constexpr const char* kProviderCostApi = "provider_cost_api";
constexpr const char* kProviderGenerationApi = "provider_generation_api";
constexpr const char* kProviderModelsApi = "provider_models_api";
constexpr const char* kOfficialDocsSnapshot = "official_docs_snapshot";
constexpr const char* kUserOverride = "user_override";
constexpr const char* kCustomContract = "custom_contract";
constexpr const char* kNone = "none";
}  // namespace source

struct CanonicalUsage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
    std::int64_t reasoning_tokens = 0;
    int request_count = 1;

    std::int64_t prompt_tokens() const noexcept;
    std::int64_t total_tokens() const noexcept;
};

struct BillingRoute {
    std::string provider;
    std::string model;
    std::string base_url;
    std::string billing_mode = "unknown";  // unknown | official_* | subscription_included
};

struct PricingEntry {
    // Per 1_000_000 tokens. std::nullopt → unknown (Python Decimal|None).
    std::optional<double> input_cost_per_million;
    std::optional<double> output_cost_per_million;
    std::optional<double> cache_read_cost_per_million;
    std::optional<double> cache_write_cost_per_million;
    std::optional<double> request_cost;
    std::string source = source::kNone;
    std::string source_url;
    std::string pricing_version;
};

struct CostResult {
    std::optional<double> amount_usd;
    std::string status = status::kUnknown;
    std::string source = source::kNone;
    std::string label;
    std::string pricing_version;
};

// Resolve which billing route applies to (model, provider, base_url).
// Understands openai/anthropic/google slashes ("anthropic/claude-…"),
// OpenRouter via base_url sniff, openai-codex subscription-included,
// and custom/local fall-throughs.
BillingRoute resolve_billing_route(const std::string& model,
                                   const std::string& provider = "",
                                   const std::string& base_url = "");

// Pure cost math: apply `entry` to `usage` and return the USD result.
// When all pricing fields are nullopt, returns a CostResult with
// status=unknown and amount_usd=nullopt.
CostResult estimate_usage_cost(const CanonicalUsage& usage,
                               const PricingEntry& entry);

// Return true when `entry` has at least one non-null pricing field.
bool has_known_pricing(const PricingEntry& entry);

// Serialise the result as JSON, matching the Python shape.
nlohmann::json cost_result_to_json(const CostResult& r);

// Serialise / deserialise PricingEntry from nlohmann::json for cache
// round-trips.
nlohmann::json pricing_entry_to_json(const PricingEntry& e);
PricingEntry pricing_entry_from_json(const nlohmann::json& j);

}  // namespace hermes::agent::pricing
