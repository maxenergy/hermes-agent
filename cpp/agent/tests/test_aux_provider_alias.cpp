#include "hermes/agent/aux_provider_alias.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::aux;

TEST(AuxProviderAlias, PassThroughUnknown) {
    EXPECT_EQ(normalize_provider("deepseek"), "deepseek");
    EXPECT_EQ(normalize_provider("ANTHROPIC"), "anthropic");
}

TEST(AuxProviderAlias, EmptyMapsToAuto) {
    EXPECT_EQ(normalize_provider(""), "auto");
    EXPECT_EQ(normalize_provider("   "), "auto");
}

TEST(AuxProviderAlias, CodexAliasesToOpenAICodex) {
    EXPECT_EQ(normalize_provider("codex"), "openai-codex");
}

TEST(AuxProviderAlias, AliasTableResolved) {
    EXPECT_EQ(normalize_provider("google"), "gemini");
    EXPECT_EQ(normalize_provider("google-ai-studio"), "gemini");
    EXPECT_EQ(normalize_provider("moonshot"), "kimi-coding");
    EXPECT_EQ(normalize_provider("claude"), "anthropic");
    EXPECT_EQ(normalize_provider("Z.AI"), "zai");
}

TEST(AuxProviderAlias, CustomColonSuffixNonVision) {
    EXPECT_EQ(normalize_provider("custom:deepseek"), "deepseek");
}

TEST(AuxProviderAlias, CustomColonSuffixVisionCollapses) {
    EXPECT_EQ(normalize_provider("custom:qwen-vl", /*vision=*/true), "custom");
}

TEST(AuxProviderAlias, CustomEmptySuffix) {
    EXPECT_EQ(normalize_provider("custom:"), "custom");
}

TEST(AuxProviderAlias, MainLeftAloneForCaller) {
    EXPECT_EQ(normalize_provider("main"), "main");
}

TEST(AuxProviderAlias, AliasTargetLookup) {
    EXPECT_EQ(alias_target("moonshot").value_or(""), "kimi-coding");
    EXPECT_FALSE(alias_target("openai").has_value());
}

TEST(AuxProviderAlias, DefaultModelLookup) {
    EXPECT_EQ(default_model_for("openai"), "gpt-4o-mini");
    EXPECT_EQ(default_model_for("Anthropic"), "claude-haiku-4");
    EXPECT_EQ(default_model_for("unknown"), "");
}
