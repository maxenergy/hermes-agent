// Tests for `hermes::cli::model_normalize`, mirroring the docstring
// examples from `hermes_cli/model_normalize.py`.

#include <gtest/gtest.h>

#include "hermes/cli/model_normalize.hpp"

using namespace hermes::cli::model_normalize;

namespace {

class ModelNormalizeResolverGuard {
 public:
    ModelNormalizeResolverGuard() { set_provider_alias_resolver(nullptr); }
    ~ModelNormalizeResolverGuard() { set_provider_alias_resolver(nullptr); }
};

}  // namespace

TEST(ModelNormalize, StripVendorPrefixSplitsOnSlash) {
    EXPECT_EQ(strip_vendor_prefix("anthropic/claude-sonnet-4.6"),
              "claude-sonnet-4.6");
    EXPECT_EQ(strip_vendor_prefix("claude-sonnet-4.6"), "claude-sonnet-4.6");
    EXPECT_EQ(strip_vendor_prefix("meta-llama/llama-4-scout"),
              "llama-4-scout");
}

TEST(ModelNormalize, StripVendorPrefixOnlyFirstSlash) {
    EXPECT_EQ(strip_vendor_prefix("a/b/c"), "b/c");
}

TEST(ModelNormalize, DotsToHyphens) {
    EXPECT_EQ(dots_to_hyphens("claude-sonnet-4.6"), "claude-sonnet-4-6");
    EXPECT_EQ(dots_to_hyphens("gpt-5.4.1"), "gpt-5-4-1");
    EXPECT_EQ(dots_to_hyphens("no-dots"), "no-dots");
}

TEST(ModelNormalize, DetectVendorKnownPrefixes) {
    EXPECT_EQ(detect_vendor("claude-sonnet-4.6"), std::optional<std::string>{"anthropic"});
    EXPECT_EQ(detect_vendor("gpt-5.4-mini"), std::optional<std::string>{"openai"});
    EXPECT_EQ(detect_vendor("o3-mini"), std::optional<std::string>{"openai"});
    EXPECT_EQ(detect_vendor("gemini-1.5"), std::optional<std::string>{"google"});
    EXPECT_EQ(detect_vendor("gemma-7b"), std::optional<std::string>{"google"});
    EXPECT_EQ(detect_vendor("deepseek-chat"), std::optional<std::string>{"deepseek"});
    EXPECT_EQ(detect_vendor("glm-5.1"), std::optional<std::string>{"z-ai"});
    EXPECT_EQ(detect_vendor("kimi-k2"), std::optional<std::string>{"moonshotai"});
    EXPECT_EQ(detect_vendor("minimax-m2"), std::optional<std::string>{"minimax"});
    EXPECT_EQ(detect_vendor("grok-3"), std::optional<std::string>{"x-ai"});
    EXPECT_EQ(detect_vendor("qwen-max"), std::optional<std::string>{"qwen"});
    EXPECT_EQ(detect_vendor("mimo-xl"), std::optional<std::string>{"xiaomi"});
    EXPECT_EQ(detect_vendor("nemotron-70b"), std::optional<std::string>{"nvidia"});
    EXPECT_EQ(detect_vendor("llama-4-scout"), std::optional<std::string>{"meta-llama"});
    EXPECT_EQ(detect_vendor("step-2"), std::optional<std::string>{"stepfun"});
    EXPECT_EQ(detect_vendor("trinity-7b"), std::optional<std::string>{"arcee-ai"});
}

TEST(ModelNormalize, DetectVendorFromExistingPrefix) {
    EXPECT_EQ(detect_vendor("anthropic/claude-sonnet-4.6"),
              std::optional<std::string>{"anthropic"});
    EXPECT_EQ(detect_vendor("MyVendor/model"),
              std::optional<std::string>{"myvendor"});
}

TEST(ModelNormalize, DetectVendorUnknown) {
    EXPECT_EQ(detect_vendor("my-custom-model"), std::nullopt);
    EXPECT_EQ(detect_vendor(""), std::nullopt);
    EXPECT_EQ(detect_vendor("   "), std::nullopt);
}

TEST(ModelNormalize, DetectVendorStartsWithPrefix) {
    // qwen3.5-plus starts with "qwen" -> should detect.
    EXPECT_EQ(detect_vendor("qwen3.5-plus"), std::optional<std::string>{"qwen"});
}

TEST(ModelNormalize, PrependVendorAddsDetectedSlug) {
    EXPECT_EQ(prepend_vendor("claude-sonnet-4.6"),
              "anthropic/claude-sonnet-4.6");
    EXPECT_EQ(prepend_vendor("gpt-5.4"), "openai/gpt-5.4");
}

TEST(ModelNormalize, PrependVendorPassesThroughExistingSlash) {
    EXPECT_EQ(prepend_vendor("anthropic/claude-sonnet-4.6"),
              "anthropic/claude-sonnet-4.6");
    EXPECT_EQ(prepend_vendor("my-custom-thing"), "my-custom-thing");
}

TEST(ModelNormalize, StripMatchingProviderPrefixMatches) {
    ModelNormalizeResolverGuard guard{};
    EXPECT_EQ(strip_matching_provider_prefix("zai/glm-5.1", "zai"),
              "glm-5.1");
}

TEST(ModelNormalize, StripMatchingProviderPrefixIgnoresMismatch) {
    ModelNormalizeResolverGuard guard{};
    EXPECT_EQ(strip_matching_provider_prefix("openai/gpt-5", "anthropic"),
              "openai/gpt-5");
}

TEST(ModelNormalize, StripMatchingProviderPrefixNoSlashNoChange) {
    ModelNormalizeResolverGuard guard{};
    EXPECT_EQ(strip_matching_provider_prefix("gpt-5", "openai"), "gpt-5");
}

TEST(ModelNormalize, StripMatchingProviderEmptyPartsKeep) {
    ModelNormalizeResolverGuard guard{};
    EXPECT_EQ(strip_matching_provider_prefix("/x", "x"), "/x");
    EXPECT_EQ(strip_matching_provider_prefix("x/", "x"), "x/");
}

TEST(ModelNormalize, DeepseekReasonerKeywords) {
    EXPECT_EQ(normalize_for_deepseek("deepseek-r1"), "deepseek-reasoner");
    EXPECT_EQ(normalize_for_deepseek("deepseek-v3-think"),
              "deepseek-reasoner");
    EXPECT_EQ(normalize_for_deepseek("my-reasoning-model"),
              "deepseek-reasoner");
    EXPECT_EQ(normalize_for_deepseek("cot-model"), "deepseek-reasoner");
}

TEST(ModelNormalize, DeepseekCanonicalPassthrough) {
    EXPECT_EQ(normalize_for_deepseek("deepseek-chat"), "deepseek-chat");
    EXPECT_EQ(normalize_for_deepseek("deepseek-reasoner"),
              "deepseek-reasoner");
}

TEST(ModelNormalize, DeepseekDefaultIsChat) {
    EXPECT_EQ(normalize_for_deepseek("deepseek-v3"), "deepseek-chat");
    EXPECT_EQ(normalize_for_deepseek("some-other-name"), "deepseek-chat");
}

TEST(ModelNormalize, NormalizeAggregatorAddsVendor) {
    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "openrouter"),
              "anthropic/claude-sonnet-4.6");
    EXPECT_EQ(normalize_model_for_provider("gpt-5.4", "nous"),
              "openai/gpt-5.4");
    EXPECT_EQ(normalize_model_for_provider("grok-3", "ai-gateway"),
              "x-ai/grok-3");
    EXPECT_EQ(normalize_model_for_provider("qwen-max", "kilocode"),
              "qwen/qwen-max");
}

TEST(ModelNormalize, NormalizeAggregatorPreservesExistingSlug) {
    EXPECT_EQ(normalize_model_for_provider("anthropic/claude-sonnet-4.6",
                                           "openrouter"),
              "anthropic/claude-sonnet-4.6");
}

TEST(ModelNormalize, NormalizeAnthropicStripsMatchingPrefix) {
    EXPECT_EQ(normalize_model_for_provider("anthropic/claude-sonnet-4.6",
                                           "anthropic"),
              "claude-sonnet-4-6");
}

TEST(ModelNormalize, NormalizeAnthropicDotsBecomeHyphens) {
    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "anthropic"),
              "claude-sonnet-4-6");
}

TEST(ModelNormalize, NormalizeAnthropicKeepsForeignSlug) {
    // Foreign prefix should NOT be stripped on anthropic provider.
    EXPECT_EQ(normalize_model_for_provider("openai/gpt-5", "anthropic"),
              "openai/gpt-5");
}

TEST(ModelNormalize, NormalizeOpencodeZenDotsBecomeHyphens) {
    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "opencode-zen"),
              "claude-sonnet-4-6");
}

TEST(ModelNormalize, NormalizeCopilotStripsMatchingPrefix) {
    EXPECT_EQ(normalize_model_for_provider("anthropic/claude-sonnet-4.6",
                                           "copilot"),
              "anthropic/claude-sonnet-4.6");  // no match -> unchanged
    EXPECT_EQ(normalize_model_for_provider("copilot/claude-sonnet-4.6",
                                           "copilot"),
              "claude-sonnet-4.6");
}

TEST(ModelNormalize, NormalizeCopilotPreservesDots) {
    EXPECT_EQ(normalize_model_for_provider("gpt-5.4", "copilot"),
              "gpt-5.4");
    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "copilot-acp"),
              "claude-sonnet-4.6");
}

TEST(ModelNormalize, NormalizeDeepseekMapsReasoner) {
    EXPECT_EQ(normalize_model_for_provider("deepseek-r1", "deepseek"),
              "deepseek-reasoner");
    EXPECT_EQ(normalize_model_for_provider("deepseek-v3", "deepseek"),
              "deepseek-chat");
}

TEST(ModelNormalize, NormalizeDeepseekStripsMatchingPrefix) {
    EXPECT_EQ(normalize_model_for_provider("deepseek/deepseek-chat",
                                           "deepseek"),
              "deepseek-chat");
}

TEST(ModelNormalize, NormalizeDeepseekKeepsForeignSlug) {
    EXPECT_EQ(normalize_model_for_provider("openai/gpt-5", "deepseek"),
              "openai/gpt-5");
}

TEST(ModelNormalize, NormalizeCustomPassthrough) {
    EXPECT_EQ(normalize_model_for_provider("my-model", "custom"), "my-model");
}

TEST(ModelNormalize, NormalizeZaiKeepsForeignSlug) {
    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "zai"),
              "claude-sonnet-4.6");
}

TEST(ModelNormalize, NormalizeZaiStripsMatchingPrefix) {
    EXPECT_EQ(normalize_model_for_provider("zai/glm-5.1", "zai"), "glm-5.1");
}

TEST(ModelNormalize, NormalizeAuthoritativeNativePassthrough) {
    EXPECT_EQ(normalize_model_for_provider("gemini-1.5-pro", "gemini"),
              "gemini-1.5-pro");
    EXPECT_EQ(normalize_model_for_provider("meta-llama/Llama-3",
                                           "huggingface"),
              "meta-llama/Llama-3");
    EXPECT_EQ(normalize_model_for_provider("codex-mini", "openai-codex"),
              "codex-mini");
}

TEST(ModelNormalize, NormalizeEmptyInput) {
    EXPECT_EQ(normalize_model_for_provider("", "openrouter"), "");
    EXPECT_EQ(normalize_model_for_provider("   ", "openrouter"), "");
}

TEST(ModelNormalize, NormalizeUnknownProviderPassthrough) {
    EXPECT_EQ(normalize_model_for_provider("any-model", "weirdprovider"),
              "any-model");
}

TEST(ModelNormalize, ClassificationHelpers) {
    EXPECT_TRUE(is_aggregator_provider("openrouter"));
    EXPECT_TRUE(is_aggregator_provider("nous"));
    EXPECT_FALSE(is_aggregator_provider("anthropic"));

    EXPECT_TRUE(is_dot_to_hyphen_provider("anthropic"));
    EXPECT_TRUE(is_dot_to_hyphen_provider("opencode-zen"));
    EXPECT_FALSE(is_dot_to_hyphen_provider("copilot"));

    EXPECT_TRUE(is_strip_vendor_only_provider("copilot"));
    EXPECT_TRUE(is_strip_vendor_only_provider("copilot-acp"));
    EXPECT_FALSE(is_strip_vendor_only_provider("anthropic"));

    EXPECT_TRUE(is_authoritative_native_provider("gemini"));
    EXPECT_FALSE(is_authoritative_native_provider("anthropic"));

    EXPECT_TRUE(is_matching_prefix_strip_provider("zai"));
    EXPECT_TRUE(is_matching_prefix_strip_provider("custom"));
    EXPECT_FALSE(is_matching_prefix_strip_provider("openrouter"));
}

TEST(ModelNormalize, CustomResolverOverridesDefault) {
    set_provider_alias_resolver([](const std::string& raw) {
        // Treat "anthropics" as "anthropic".
        if (raw == "anthropics" || raw == "Anthropics") {
            return std::string{"anthropic"};
        }
        std::string out{raw};
        for (auto& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    });

    EXPECT_EQ(normalize_model_for_provider("claude-sonnet-4.6", "Anthropics"),
              "claude-sonnet-4-6");

    set_provider_alias_resolver(nullptr);
}
