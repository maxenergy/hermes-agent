#include "hermes/llm/model_normalize.hpp"

#include <gtest/gtest.h>

using hermes::llm::normalize_model_id;
using hermes::llm::is_codex_model;

TEST(ModelNormalize, SinglePrefix) {
    EXPECT_EQ("claude-opus-4-6",
              normalize_model_id("anthropic/claude-opus-4-6"));
}

TEST(ModelNormalize, NestedPrefix) {
    EXPECT_EQ("claude-opus-4-6",
              normalize_model_id("openrouter/anthropic/claude-opus-4-6"));
}

TEST(ModelNormalize, NoPrefix) {
    EXPECT_EQ("claude-opus-4-6",
              normalize_model_id("claude-opus-4-6"));
}

TEST(ModelNormalize, OllamaStyleTag) {
    // rfind('/') finds no slash, so the full string is returned.
    EXPECT_EQ("llama3:70b", normalize_model_id("llama3:70b"));
}

TEST(ModelNormalize, TripleNestedPrefix) {
    EXPECT_EQ("gpt-4o",
              normalize_model_id("a/b/c/gpt-4o"));
}

TEST(IsCodexModel, ClaudeFamily) {
    EXPECT_TRUE(is_codex_model("claude-opus-4-6"));
    EXPECT_TRUE(is_codex_model("anthropic/claude-sonnet-4-6"));
    EXPECT_TRUE(is_codex_model("openrouter/anthropic/claude-haiku"));
}

TEST(IsCodexModel, GptFamily) {
    EXPECT_TRUE(is_codex_model("gpt-4o"));
    EXPECT_TRUE(is_codex_model("gpt-4o-mini"));
    EXPECT_TRUE(is_codex_model("gpt-4.1-turbo"));
    EXPECT_TRUE(is_codex_model("gpt-5"));
}

TEST(IsCodexModel, CodexPrefix) {
    EXPECT_TRUE(is_codex_model("codex-mini"));
    EXPECT_TRUE(is_codex_model("code-davinci-002"));
}

TEST(IsCodexModel, DeepseekCoder) {
    EXPECT_TRUE(is_codex_model("deepseek-coder-v2"));
}

TEST(IsCodexModel, QwenCoder) {
    EXPECT_TRUE(is_codex_model("qwen3-coder"));
}

TEST(IsCodexModel, NonCodeModel) {
    EXPECT_FALSE(is_codex_model("llama3:70b"));
    EXPECT_FALSE(is_codex_model("gemma-7b"));
    EXPECT_FALSE(is_codex_model("mistral-7b"));
}
