// Tests for hermes::agent::pricing.
#include "hermes/agent/usage_pricing.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::pricing;

TEST(UsagePricing, CanonicalTotals) {
    CanonicalUsage u;
    u.input_tokens = 100;
    u.cache_read_tokens = 50;
    u.cache_write_tokens = 10;
    u.output_tokens = 25;
    EXPECT_EQ(u.prompt_tokens(), 160);
    EXPECT_EQ(u.total_tokens(), 185);
}

TEST(UsagePricing, RouteOpenRouterByBaseUrl) {
    auto r = resolve_billing_route("any-model", "",
                                   "https://openrouter.ai/api/v1");
    EXPECT_EQ(r.provider, "openrouter");
    EXPECT_EQ(r.billing_mode, "official_models_api");
}

TEST(UsagePricing, RouteAnthropicFromSlashPrefix) {
    auto r = resolve_billing_route("anthropic/claude-sonnet-4-20250514");
    EXPECT_EQ(r.provider, "anthropic");
    EXPECT_EQ(r.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(r.billing_mode, "official_docs_snapshot");
}

TEST(UsagePricing, RouteOpenAIPlain) {
    auto r = resolve_billing_route("gpt-4o-mini", "openai");
    EXPECT_EQ(r.provider, "openai");
    EXPECT_EQ(r.billing_mode, "official_docs_snapshot");
}

TEST(UsagePricing, RouteOpenAICodexSubscription) {
    auto r = resolve_billing_route("gpt-5", "openai-codex");
    EXPECT_EQ(r.billing_mode, "subscription_included");
}

TEST(UsagePricing, RouteLocalhostCustom) {
    auto r = resolve_billing_route("dev-model", "", "http://localhost:11434");
    EXPECT_EQ(r.provider, "custom");
    EXPECT_EQ(r.billing_mode, "unknown");
}

TEST(UsagePricing, HasKnownPricing) {
    PricingEntry e;
    EXPECT_FALSE(has_known_pricing(e));
    e.input_cost_per_million = 3.0;
    EXPECT_TRUE(has_known_pricing(e));
}

TEST(UsagePricing, EstimateSimpleCost) {
    PricingEntry e;
    e.input_cost_per_million = 3.0;
    e.output_cost_per_million = 15.0;
    e.source = source::kOfficialDocsSnapshot;
    CanonicalUsage u;
    u.input_tokens = 1000000;
    u.output_tokens = 500000;
    auto r = estimate_usage_cost(u, e);
    ASSERT_TRUE(r.amount_usd.has_value());
    EXPECT_NEAR(*r.amount_usd, 3.0 + 7.5, 1e-9);
    EXPECT_EQ(r.status, status::kEstimated);
    EXPECT_EQ(r.source, source::kOfficialDocsSnapshot);
}

TEST(UsagePricing, EstimateNoPricingIsUnknown) {
    PricingEntry e;
    CanonicalUsage u;
    u.input_tokens = 1000;
    auto r = estimate_usage_cost(u, e);
    EXPECT_FALSE(r.amount_usd.has_value());
    EXPECT_EQ(r.status, status::kUnknown);
}

TEST(UsagePricing, EstimateAllZeroIsIncluded) {
    PricingEntry e;
    e.input_cost_per_million = 0.0;
    e.output_cost_per_million = 0.0;
    CanonicalUsage u;
    u.input_tokens = 1000000;
    u.output_tokens = 1000000;
    auto r = estimate_usage_cost(u, e);
    ASSERT_TRUE(r.amount_usd.has_value());
    EXPECT_DOUBLE_EQ(*r.amount_usd, 0.0);
    EXPECT_EQ(r.status, status::kIncluded);
}

TEST(UsagePricing, CacheReadAndWriteFactoredIn) {
    PricingEntry e;
    e.input_cost_per_million = 3.0;
    e.output_cost_per_million = 15.0;
    e.cache_read_cost_per_million = 0.30;
    e.cache_write_cost_per_million = 3.75;
    CanonicalUsage u;
    u.cache_read_tokens = 1000000;
    u.cache_write_tokens = 1000000;
    auto r = estimate_usage_cost(u, e);
    ASSERT_TRUE(r.amount_usd.has_value());
    EXPECT_NEAR(*r.amount_usd, 0.30 + 3.75, 1e-9);
}

TEST(UsagePricing, JsonRoundTrip) {
    PricingEntry e;
    e.input_cost_per_million = 2.0;
    e.output_cost_per_million = 10.0;
    e.source = source::kOfficialDocsSnapshot;
    e.pricing_version = "anthropic-2026-03";
    auto j = pricing_entry_to_json(e);
    auto back = pricing_entry_from_json(j);
    EXPECT_EQ(*back.input_cost_per_million, 2.0);
    EXPECT_EQ(*back.output_cost_per_million, 10.0);
    EXPECT_FALSE(back.cache_read_cost_per_million.has_value());
    EXPECT_EQ(back.source, source::kOfficialDocsSnapshot);
    EXPECT_EQ(back.pricing_version, "anthropic-2026-03");
}

TEST(UsagePricing, CostResultJsonShape) {
    CostResult r;
    r.amount_usd = 0.0123;
    r.status = status::kEstimated;
    r.source = source::kProviderModelsApi;
    r.label = "ok";
    auto j = cost_result_to_json(r);
    EXPECT_DOUBLE_EQ(j["amount_usd"].get<double>(), 0.0123);
    EXPECT_EQ(j["status"], status::kEstimated);
    EXPECT_EQ(j["source"], source::kProviderModelsApi);
}

TEST(UsagePricing, CostResultJsonHandlesNull) {
    CostResult r;
    auto j = cost_result_to_json(r);
    EXPECT_TRUE(j["amount_usd"].is_null());
    EXPECT_EQ(j["status"], status::kUnknown);
}
