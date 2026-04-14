// Unit tests for tokenizer.hpp family detection, token estimation, and
// expanded model registry lookup.
#include "hermes/llm/tokenizer.hpp"
#include "hermes/llm/message.hpp"

#include <gtest/gtest.h>

using namespace hermes::llm;

TEST(Tokenizer, FamilyDetectionClaude) {
    EXPECT_EQ(family_of("claude-opus-4-6"),              ModelFamily::Claude);
    EXPECT_EQ(family_of("anthropic/claude-sonnet-4-6"),  ModelFamily::Claude);
    EXPECT_EQ(family_of("claude-3-5-sonnet-20241022"),   ModelFamily::Claude);
}

TEST(Tokenizer, FamilyDetectionGpt) {
    EXPECT_EQ(family_of("gpt-4o"),             ModelFamily::Gpt);
    EXPECT_EQ(family_of("gpt-5"),              ModelFamily::Gpt);
    EXPECT_EQ(family_of("o1-preview"),         ModelFamily::Gpt);
    EXPECT_EQ(family_of("o3-mini-high"),       ModelFamily::Gpt);
    EXPECT_EQ(family_of("gpt-5.3-codex"),      ModelFamily::Gpt);
}

TEST(Tokenizer, FamilyDetectionOthers) {
    EXPECT_EQ(family_of("gemini-2.5-pro"),     ModelFamily::Gemini);
    EXPECT_EQ(family_of("meta/llama-3.1-70b"), ModelFamily::Llama);
    EXPECT_EQ(family_of("mistralai/mixtral"),  ModelFamily::Mistral);
    EXPECT_EQ(family_of("qwen3-max"),          ModelFamily::Qwen);
    EXPECT_EQ(family_of("deepseek-reasoner"),  ModelFamily::Deepseek);
    EXPECT_EQ(family_of("glm-4.5"),            ModelFamily::Glm);
    EXPECT_EQ(family_of("kimi-k2"),            ModelFamily::Kimi);
    EXPECT_EQ(family_of("minimax-m2"),         ModelFamily::Minimax);
    EXPECT_EQ(family_of("grok-4"),             ModelFamily::Grok);
    EXPECT_EQ(family_of("some-unknown-model"), ModelFamily::Unknown);
}

TEST(Tokenizer, CharsPerTokenByFamily) {
    EXPECT_NEAR(chars_per_token(ModelFamily::Claude),   3.5, 1e-9);
    EXPECT_NEAR(chars_per_token(ModelFamily::Gpt),      4.0, 1e-9);
    EXPECT_NEAR(chars_per_token(ModelFamily::Qwen),     2.8, 1e-9);
    EXPECT_NEAR(chars_per_token(ModelFamily::Unknown),  4.0, 1e-9);
}

TEST(Tokenizer, EstimateTokensSimple) {
    // 40 chars, claude ratio 3.5 → ceil(40/3.5) = 12
    std::string text(40, 'a');
    EXPECT_EQ(estimate_tokens(text, ModelFamily::Claude), 12);
    // 40 chars, gpt ratio 4.0 → 10
    EXPECT_EQ(estimate_tokens(text, ModelFamily::Gpt), 10);
    // Empty input.
    EXPECT_EQ(estimate_tokens("", ModelFamily::Claude), 0);
}

TEST(Tokenizer, CountTokensMessagesOverhead) {
    Message u;
    u.role = Role::User;
    u.content_text = std::string(40, 'x');  // 10 tokens in GPT family
    std::vector<Message> msgs = {u};
    // 4 per-message overhead + 10 content + 2 priming = 16
    EXPECT_EQ(count_tokens_messages(msgs, ModelFamily::Gpt), 16);
}

TEST(Tokenizer, CountTokensMessagesMultipleRoles) {
    Message sys, u, a;
    sys.role = Role::System;
    sys.content_text = std::string(20, 'a');  // 5 tokens
    u.role = Role::User;
    u.content_text = std::string(40, 'b');    // 10 tokens
    a.role = Role::Assistant;
    a.content_text = std::string(80, 'c');    // 20 tokens
    std::vector<Message> msgs = {sys, u, a};
    // 3 * 4 overhead + 35 content + 2 priming = 49
    EXPECT_EQ(count_tokens_messages(msgs, ModelFamily::Gpt), 49);
}

TEST(Tokenizer, LookupModelInfoOpus46) {
    auto info = lookup_model_info("claude-opus-4-6-20251022");
    EXPECT_EQ(info.family, ModelFamily::Claude);
    EXPECT_EQ(info.context_length, 1'000'000);
    EXPECT_EQ(info.max_output_tokens, 128'000);
    EXPECT_TRUE(info.supports_reasoning);
    EXPECT_TRUE(info.supports_vision);
    EXPECT_TRUE(info.supports_prompt_cache);
    EXPECT_NEAR(info.pricing.input_per_million_usd,  15.00, 1e-9);
    EXPECT_NEAR(info.pricing.output_per_million_usd, 75.00, 1e-9);
    EXPECT_NEAR(info.pricing.cache_read_per_million_usd,  1.50, 1e-9);
}

TEST(Tokenizer, LookupModelInfoHaiku45NoReasoning) {
    auto info = lookup_model_info("claude-haiku-4-5-20251001");
    EXPECT_EQ(info.family, ModelFamily::Claude);
    EXPECT_EQ(info.context_length, 200'000);
    EXPECT_FALSE(info.supports_reasoning);
    EXPECT_TRUE(info.supports_prompt_cache);
}

TEST(Tokenizer, LookupModelInfoGpt5) {
    auto info = lookup_model_info("gpt-5");
    EXPECT_EQ(info.family, ModelFamily::Gpt);
    EXPECT_GE(info.context_length, 400'000);
    EXPECT_TRUE(info.supports_reasoning);
    EXPECT_TRUE(info.supports_vision);
}

TEST(Tokenizer, LookupModelInfoUnknownFallsBackToFamilyDefaults) {
    auto info = lookup_model_info("llama-3.1-custom-tune");
    EXPECT_EQ(info.family, ModelFamily::Llama);
    EXPECT_EQ(info.context_length, 128'000);
    EXPECT_EQ(info.max_output_tokens, 8192);
}

TEST(Tokenizer, LookupModelInfoTotallyUnknown) {
    auto info = lookup_model_info("some-totally-fake-model");
    EXPECT_EQ(info.family, ModelFamily::Unknown);
    EXPECT_EQ(info.context_length, 8192);
}

TEST(Tokenizer, LookupModelInfoLongestPrefixWins) {
    // "claude-opus-4-6" must beat "claude-opus" and "claude-opus-4".
    auto opus46 = lookup_model_info("claude-opus-4-6");
    auto opus4  = lookup_model_info("claude-opus-4-20250501");
    EXPECT_EQ(opus46.context_length, 1'000'000);
    EXPECT_EQ(opus4.context_length,    200'000);
}

TEST(Tokenizer, BudgetPlanStandard) {
    auto p = plan_budget("claude-sonnet-4-6", 16'000, 1024);
    EXPECT_FALSE(p.clamped);
    EXPECT_EQ(p.output_budget, 16'000);
    EXPECT_EQ(p.safety_margin, 1024);
    // context 1,000,000 − 16,000 − 1024 = 982,976
    EXPECT_EQ(p.input_budget, 982'976);
}

TEST(Tokenizer, BudgetPlanClampsLargeDesired) {
    // gpt-4o has max_output 16384; request 64k gets clamped.
    auto p = plan_budget("gpt-4o", 64'000, 512);
    EXPECT_TRUE(p.clamped);
    EXPECT_EQ(p.output_budget, 16'384);
}

TEST(Tokenizer, BudgetPlanNegativeDesiredUsesNativeMax) {
    auto p = plan_budget("gpt-4o-mini", 0, 0);
    EXPECT_EQ(p.output_budget, 16'384);
}

TEST(Tokenizer, ListKnownModelsNonEmpty) {
    auto list = list_known_models();
    EXPECT_GT(list.size(), 20u);
}

TEST(Tokenizer, FamilyNameRoundTrip) {
    EXPECT_EQ(std::string(family_name(ModelFamily::Claude)),  "claude");
    EXPECT_EQ(std::string(family_name(ModelFamily::Gpt)),     "gpt");
    EXPECT_EQ(std::string(family_name(ModelFamily::Unknown)), "unknown");
}
