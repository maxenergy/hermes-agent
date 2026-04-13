#include "hermes/llm/codex_models.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using hermes::llm::add_forward_compat_models;
using hermes::llm::default_codex_models;
using hermes::llm::is_codex_backed_model;

TEST(CodexModels, DefaultsPopulated) {
    const auto& defaults = default_codex_models();
    EXPECT_FALSE(defaults.empty());
    EXPECT_NE(std::find(defaults.begin(), defaults.end(), "gpt-5.3-codex"),
              defaults.end());
    EXPECT_NE(std::find(defaults.begin(), defaults.end(), "gpt-5.1-codex-mini"),
              defaults.end());
}

TEST(CodexModels, IsCodexBacked) {
    EXPECT_TRUE(is_codex_backed_model("gpt-5.3-codex"));
    EXPECT_TRUE(is_codex_backed_model("gpt-5.1-codex-mini"));
    EXPECT_TRUE(is_codex_backed_model("gpt-5.4-mini"));
    EXPECT_TRUE(is_codex_backed_model("gpt-5.3-codex-spark"));
    // Prefix stripping.
    EXPECT_TRUE(is_codex_backed_model("openai/gpt-5.3-codex"));
    EXPECT_TRUE(is_codex_backed_model("openrouter/openai/gpt-5.3-codex"));

    // Non-Codex models must NOT be reported as Codex-backed.
    EXPECT_FALSE(is_codex_backed_model("gpt-4o"));
    EXPECT_FALSE(is_codex_backed_model("gpt-4o-mini"));
    EXPECT_FALSE(is_codex_backed_model("claude-opus-4-6"));
    EXPECT_FALSE(is_codex_backed_model("qwen3-coder-plus"));
    EXPECT_FALSE(is_codex_backed_model("deepseek-coder-v2"));
}

TEST(CodexModels, IsCodexBackedMatchesHypotheticalGpt5Codex) {
    // New synthetic slug we haven't seen yet — still matches the "gpt-5" +
    // "codex" heuristic.
    EXPECT_TRUE(is_codex_backed_model("gpt-5.9-codex-ultra"));
}

TEST(CodexModels, ForwardCompatInjectsSyntheticSlugs) {
    // Starting set contains an older template — forward-compat adds the
    // newer slugs.
    std::vector<std::string> input = {"gpt-5.2-codex"};
    auto out = add_forward_compat_models(input);
    EXPECT_EQ(out[0], "gpt-5.2-codex");
    // Each synthetic whose templates include gpt-5.2-codex is injected.
    EXPECT_NE(std::find(out.begin(), out.end(), "gpt-5.4-mini"), out.end());
    EXPECT_NE(std::find(out.begin(), out.end(), "gpt-5.4"), out.end());
    EXPECT_NE(std::find(out.begin(), out.end(), "gpt-5.3-codex"), out.end());
    EXPECT_NE(std::find(out.begin(), out.end(), "gpt-5.3-codex-spark"), out.end());
}

TEST(CodexModels, ForwardCompatPreservesOrderAndDeduplicates) {
    std::vector<std::string> input = {"gpt-5.3-codex", "gpt-5.3-codex",
                                      "gpt-5.2-codex"};
    auto out = add_forward_compat_models(input);
    EXPECT_EQ(out[0], "gpt-5.3-codex");
    EXPECT_EQ(out[1], "gpt-5.2-codex");
    // No duplicate
    EXPECT_EQ(std::count(out.begin(), out.end(), "gpt-5.3-codex"), 1);
    // gpt-5.3-codex already present → not re-injected.
}

TEST(CodexModels, ForwardCompatNoopWhenNoTemplatesPresent) {
    std::vector<std::string> input = {"claude-opus-4-6", "gpt-4o"};
    auto out = add_forward_compat_models(input);
    // No synthetic slug should be injected because no codex template is
    // present.
    EXPECT_EQ(out.size(), input.size());
}
