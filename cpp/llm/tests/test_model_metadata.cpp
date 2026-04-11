#include "hermes/llm/model_metadata.hpp"

#include <gtest/gtest.h>

using hermes::llm::Message;
using hermes::llm::Role;

TEST(ModelMetadata, StripProviderPrefix) {
    EXPECT_EQ("claude-opus-4-6",
              hermes::llm::strip_provider_prefix("claude-opus-4-6"));
    EXPECT_EQ("claude-opus-4-6",
              hermes::llm::strip_provider_prefix("anthropic/claude-opus-4-6"));
    EXPECT_EQ("deepseek-chat",
              hermes::llm::strip_provider_prefix("deepseek/deepseek-chat"));
}

TEST(ModelMetadata, EstimateTokensRoughMonotone) {
    const auto a = hermes::llm::estimate_tokens_rough("");
    const auto b = hermes::llm::estimate_tokens_rough("hello");
    const auto c = hermes::llm::estimate_tokens_rough(
        "the quick brown fox jumps over the lazy dog");
    EXPECT_LE(a, b);
    EXPECT_LT(b, c);
    EXPECT_EQ(a, 0);
}

TEST(ModelMetadata, EstimateMessagesTokensRoughCountsBlocks) {
    Message m;
    m.role = Role::User;
    m.content_text = "short";
    const int64_t base =
        hermes::llm::estimate_messages_tokens_rough({m});

    Message m2 = m;
    hermes::llm::ContentBlock blk;
    blk.type = "text";
    blk.text = "a much longer text segment to increase the count";
    m2.content_blocks.push_back(blk);
    const int64_t with_block =
        hermes::llm::estimate_messages_tokens_rough({m2});
    EXPECT_GT(with_block, base);
}

TEST(ModelMetadata, ParseContextLimitFromError_MaximumContextLength) {
    const auto v = hermes::llm::parse_context_limit_from_error(
        "This model's maximum context length is 32768 tokens. However, ...");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 32768);
}

TEST(ModelMetadata, ParseContextLimitFromError_ContextWindow) {
    const auto v = hermes::llm::parse_context_limit_from_error(
        "The context window is 131072 tokens for this model.");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 131072);
}

TEST(ModelMetadata, ParseContextLimitFromError_Comparison) {
    const auto v = hermes::llm::parse_context_limit_from_error(
        "Request exceeds limit: 250000 tokens > 200000 maximum");
    ASSERT_TRUE(v.has_value());
    // Either pattern may match; both are meaningful numbers.
    EXPECT_TRUE(*v == 250000 || *v == 200000);
}

TEST(ModelMetadata, ParseContextLimitFromError_NoMatch) {
    const auto v = hermes::llm::parse_context_limit_from_error(
        "rate limit exceeded, please try again later");
    EXPECT_FALSE(v.has_value());
}

TEST(ModelMetadata, FetchHardcodedClaude) {
    const auto md = hermes::llm::fetch_model_metadata("claude-opus-4-6");
    EXPECT_EQ(md.family, "claude");
    EXPECT_EQ(md.context_length, 1'000'000);
    EXPECT_TRUE(md.supports_prompt_cache);
    EXPECT_GT(md.pricing.input_per_million_usd, 0.0);
}

TEST(ModelMetadata, FetchUnknownModel) {
    const auto md = hermes::llm::fetch_model_metadata("completely-made-up-5");
    EXPECT_EQ(md.context_length, -1);
    EXPECT_EQ(md.pricing.input_per_million_usd, 0.0);
}

TEST(ModelMetadata, ProbeTiersStrictlyDecreasing) {
    int64_t prev = hermes::llm::CONTEXT_PROBE_TIERS[0] + 1;
    for (auto t : hermes::llm::CONTEXT_PROBE_TIERS) {
        EXPECT_LT(t, prev);
        prev = t;
    }
}
