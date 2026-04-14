// Tests for hermes::cli::models_cmd.
#include "hermes/cli/models_cmd.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::models_cmd;

TEST(ModelsCmd, NormaliseProvider_AliasesMapToCanonical) {
    EXPECT_EQ(normalize_provider("glm"), "zai");
    EXPECT_EQ(normalize_provider("github-copilot"), "copilot");
    EXPECT_EQ(normalize_provider("google"), "gemini");
    EXPECT_EQ(normalize_provider("openrouter"), "openrouter");
    EXPECT_EQ(normalize_provider("Z.AI"), "zai");  // Case-insensitive.
}

TEST(ModelsCmd, ProviderLabel_ReturnsReadable) {
    EXPECT_EQ(provider_label("anthropic"), "Anthropic");
    EXPECT_EQ(provider_label("zai"), "Z.AI / GLM");
    EXPECT_EQ(provider_label("hf"), "Hugging Face");
}

TEST(ModelsCmd, CuratedModelsForProvider_NonEmpty) {
    auto m = curated_models_for_provider("anthropic");
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.front().description, "recommended");
    EXPECT_EQ(m.front().id, "claude-opus-4-6");
}

TEST(ModelsCmd, CuratedModels_FreeTagPropagates) {
    auto m = curated_models_for_provider("nous");
    bool found_free = false;
    for (const auto& x : m) {
        if (x.description == "free") { found_free = true; break; }
    }
    EXPECT_TRUE(found_free);
}

TEST(ModelsCmd, ParseModelInput_ColonSeparator) {
    auto r = parse_model_input("anthropic:claude-opus-4-6", "openai");
    EXPECT_EQ(r.provider, "anthropic");
    EXPECT_EQ(r.model, "claude-opus-4-6");
}

TEST(ModelsCmd, ParseModelInput_SlashFallbackUsesDefault) {
    // `anthropic/claude-opus-4.6` — slash with known provider should
    // split.
    auto r = parse_model_input("anthropic/claude-opus-4.6", "openai");
    EXPECT_EQ(r.provider, "anthropic");
    EXPECT_EQ(r.model, "claude-opus-4.6");
    // Unknown prefix falls back to current provider.
    auto r2 = parse_model_input("unknown/foo", "openai");
    EXPECT_EQ(r2.provider, "openai");
    EXPECT_EQ(r2.model, "unknown/foo");
}

TEST(ModelsCmd, DetectProviderForModel_FindsById) {
    EXPECT_EQ(detect_provider_for_model("claude-opus-4-6"), "anthropic");
    EXPECT_EQ(detect_provider_for_model("deepseek-chat"), "deepseek");
    EXPECT_EQ(detect_provider_for_model("xxxnever"), "");
}

TEST(ModelsCmd, StripVendorPrefix) {
    EXPECT_EQ(strip_vendor_prefix("anthropic/claude-opus"), "claude-opus");
    EXPECT_EQ(strip_vendor_prefix("no-slash"), "no-slash");
}

TEST(ModelsCmd, PricingAndContext_KnownModels) {
    auto p = pricing_for_model("anthropic/claude-opus-4.6");
    EXPECT_GT(p.prompt_per_mtok, 0.0);
    EXPECT_GT(p.completion_per_mtok, p.prompt_per_mtok);
    EXPECT_GE(context_window_for_model("anthropic/claude-opus-4.6"),
              100000);
}

TEST(ModelsCmd, FastMode_Heuristics) {
    EXPECT_TRUE(model_supports_fast_mode("claude-haiku-4.5"));
    EXPECT_TRUE(model_supports_fast_mode("gpt-5.4-mini"));
    EXPECT_FALSE(model_supports_fast_mode("claude-opus-4-6"));
}

TEST(ModelsCmd, KnownProviders_SortedAndNonEmpty) {
    auto ps = known_providers();
    ASSERT_FALSE(ps.empty());
    for (std::size_t i = 1; i < ps.size(); ++i) {
        EXPECT_LE(ps[i - 1], ps[i]);
    }
}
