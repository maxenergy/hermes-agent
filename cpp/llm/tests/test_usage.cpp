#include "hermes/llm/usage.hpp"

#include <gtest/gtest.h>

using hermes::llm::CanonicalUsage;
using hermes::llm::PricingTier;
using json = nlohmann::json;

TEST(Usage, NormalizeOpenAi) {
    const json u = {
        {"prompt_tokens", 1234},
        {"completion_tokens", 456},
        {"prompt_tokens_details", {{"cached_tokens", 500}}},
        {"completion_tokens_details", {{"reasoning_tokens", 200}}},
    };
    const CanonicalUsage cu = hermes::llm::normalize_openai_usage(u);
    EXPECT_EQ(cu.input_tokens, 1234);
    EXPECT_EQ(cu.output_tokens, 456);
    EXPECT_EQ(cu.cache_read_input_tokens, 500);
    EXPECT_EQ(cu.reasoning_tokens, 200);
}

TEST(Usage, NormalizeAnthropic) {
    const json u = {
        {"input_tokens", 100},
        {"output_tokens", 200},
        {"cache_read_input_tokens", 50},
        {"cache_creation_input_tokens", 30},
    };
    const CanonicalUsage cu = hermes::llm::normalize_anthropic_usage(u);
    EXPECT_EQ(cu.input_tokens, 100);
    EXPECT_EQ(cu.output_tokens, 200);
    EXPECT_EQ(cu.cache_read_input_tokens, 50);
    EXPECT_EQ(cu.cache_creation_input_tokens, 30);
}

TEST(Usage, EstimateCost) {
    CanonicalUsage u;
    u.input_tokens = 1'000'000;
    u.output_tokens = 500'000;
    u.cache_read_input_tokens = 100'000;
    u.cache_creation_input_tokens = 50'000;

    PricingTier p;
    p.input_per_million_usd = 3.0;
    p.output_per_million_usd = 15.0;
    p.cache_read_per_million_usd = 0.3;
    p.cache_write_per_million_usd = 3.75;

    // regular_input = 1_000_000 - 100_000 - 50_000 = 850_000 → $2.55
    // output = 500_000 → $7.50
    // cache_read = 100_000 → $0.03
    // cache_write = 50_000 → $0.1875
    // total = $10.2675
    EXPECT_NEAR(hermes::llm::estimate_usage_cost(u, p), 10.2675, 1e-6);
}

TEST(Usage, LookupPricingKnownModels) {
    for (const char* name : {
        "claude-opus-4-6", "claude-sonnet-4-6", "claude-haiku-4-5",
        "gpt-4o", "gpt-4o-mini", "deepseek-chat", "gemini-2.0-flash"}) {
        const auto p = hermes::llm::lookup_pricing(name);
        EXPECT_GT(p.input_per_million_usd, 0.0) << name;
        EXPECT_GT(p.output_per_million_usd, 0.0) << name;
    }
}

TEST(Usage, LookupPricingUnknownModelReturnsZero) {
    const auto p = hermes::llm::lookup_pricing("totally-unknown-model");
    EXPECT_EQ(p.input_per_million_usd, 0.0);
    EXPECT_EQ(p.output_per_million_usd, 0.0);
}

TEST(Usage, FormatTokenCountCompact) {
    EXPECT_EQ(hermes::llm::format_token_count_compact(500), "500");
    EXPECT_EQ(hermes::llm::format_token_count_compact(12'300), "12.3k");
    EXPECT_EQ(hermes::llm::format_token_count_compact(1'200'000), "1.2M");
}

TEST(Usage, FormatDurationCompact) {
    EXPECT_EQ(hermes::llm::format_duration_compact(std::chrono::milliseconds(450)),
              "450ms");
    EXPECT_EQ(hermes::llm::format_duration_compact(std::chrono::milliseconds(2300)),
              "2.3s");
    EXPECT_EQ(hermes::llm::format_duration_compact(std::chrono::milliseconds(74'000)),
              "1m14s");
}
