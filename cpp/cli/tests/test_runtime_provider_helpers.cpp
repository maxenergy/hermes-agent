// Tests for runtime_provider_helpers — pure-logic port of
// hermes_cli/runtime_provider.py.
#include "hermes/cli/runtime_provider_helpers.hpp"

#include <gtest/gtest.h>

namespace rp = hermes::cli::runtime_provider_helpers;

TEST(RuntimeProviderNormalise, Trims) {
    EXPECT_EQ(rp::normalize_custom_provider_name("  My Endpoint "),
              "my-endpoint");
}

TEST(RuntimeProviderNormalise, Empty) {
    EXPECT_EQ(rp::normalize_custom_provider_name(""), "");
}

TEST(RuntimeProviderDetectMode, OpenAI) {
    EXPECT_EQ(rp::detect_api_mode_for_url("https://api.openai.com/v1"),
              std::optional<std::string> {"codex_responses"});
}

TEST(RuntimeProviderDetectMode, OpenRouterExcluded) {
    EXPECT_FALSE(rp::detect_api_mode_for_url(
                     "https://openrouter.ai/api/v1").has_value());
}

TEST(RuntimeProviderDetectMode, Other) {
    EXPECT_FALSE(
        rp::detect_api_mode_for_url("https://api.anthropic.com").has_value());
}

TEST(RuntimeProviderParseMode, Valid) {
    EXPECT_EQ(*rp::parse_api_mode("chat_completions"), "chat_completions");
    EXPECT_EQ(*rp::parse_api_mode("CODEX_RESPONSES"), "codex_responses");
    EXPECT_EQ(*rp::parse_api_mode(" anthropic_messages "),
              "anthropic_messages");
}

TEST(RuntimeProviderParseMode, Invalid) {
    EXPECT_FALSE(rp::parse_api_mode("").has_value());
    EXPECT_FALSE(rp::parse_api_mode("garbage").has_value());
}

TEST(RuntimeProviderLocal, Detects) {
    EXPECT_TRUE(rp::is_local_base_url("http://localhost:8080"));
    EXPECT_TRUE(rp::is_local_base_url("http://127.0.0.1/v1"));
    EXPECT_FALSE(rp::is_local_base_url("https://api.openai.com"));
}

TEST(RuntimeProviderSupports, EmptyConfigured) {
    EXPECT_TRUE(rp::provider_supports_explicit_api_mode("copilot", ""));
}

TEST(RuntimeProviderSupports, MatchingProvider) {
    EXPECT_TRUE(rp::provider_supports_explicit_api_mode("copilot", "copilot"));
    EXPECT_FALSE(
        rp::provider_supports_explicit_api_mode("anthropic", "copilot"));
}

TEST(RuntimeProviderSupports, CustomVariants) {
    EXPECT_TRUE(rp::provider_supports_explicit_api_mode("custom", "custom"));
    EXPECT_TRUE(
        rp::provider_supports_explicit_api_mode("custom", "custom:foo"));
    EXPECT_FALSE(rp::provider_supports_explicit_api_mode("custom", "openai"));
}

TEST(RuntimeProviderDefaults, Codex) {
    EXPECT_EQ(rp::default_base_url_for_provider("openai-codex"),
              rp::kDefaultCodexBaseUrl);
    EXPECT_EQ(rp::default_api_mode_for_provider("openai-codex"),
              "codex_responses");
}

TEST(RuntimeProviderDefaults, Anthropic) {
    EXPECT_EQ(rp::default_base_url_for_provider("anthropic"),
              rp::kDefaultAnthropicBaseUrl);
    EXPECT_EQ(rp::default_api_mode_for_provider("anthropic"),
              "anthropic_messages");
}

TEST(RuntimeProviderDefaults, Unknown) {
    EXPECT_EQ(rp::default_base_url_for_provider("unknown"), "");
    EXPECT_EQ(rp::default_api_mode_for_provider("unknown"), "chat_completions");
}

TEST(RuntimeProviderStrip, TrailingSlash) {
    EXPECT_EQ(rp::strip_trailing_slash("https://x/"), "https://x");
    EXPECT_EQ(rp::strip_trailing_slash("https://x"), "https://x");
    EXPECT_EQ(rp::strip_trailing_slash(""), "");
}

TEST(RuntimeProviderStrip, V1Suffix) {
    EXPECT_EQ(rp::strip_v1_suffix("https://opencode.ai/zen/go/v1"),
              "https://opencode.ai/zen/go");
    EXPECT_EQ(rp::strip_v1_suffix("https://opencode.ai/zen/go/v1/"),
              "https://opencode.ai/zen/go");
    EXPECT_EQ(rp::strip_v1_suffix("https://example.com"),
              "https://example.com");
}

TEST(RuntimeProviderStrip, AnthropicSuffix) {
    EXPECT_TRUE(
        rp::url_hints_anthropic_mode("https://proxy/anthropic"));
    EXPECT_TRUE(
        rp::url_hints_anthropic_mode("https://proxy/anthropic/"));
    EXPECT_FALSE(rp::url_hints_anthropic_mode("https://proxy/openai"));
}

TEST(RuntimeProviderResolve, ExplicitWins) {
    EXPECT_EQ(rp::resolve_requested_provider("COPILOT", "anthropic", "nous"),
              "copilot");
}

TEST(RuntimeProviderResolve, ConfigBeatsEnv) {
    EXPECT_EQ(rp::resolve_requested_provider("", "anthropic", "openai"),
              "anthropic");
}

TEST(RuntimeProviderResolve, FallsBackToEnv) {
    EXPECT_EQ(rp::resolve_requested_provider("", "", "NOUS"), "nous");
}

TEST(RuntimeProviderResolve, Auto) {
    EXPECT_EQ(rp::resolve_requested_provider("", "", ""), "auto");
}

TEST(RuntimeProviderFormatErr, AuthError) {
    rp::ErrorContext ctx {};
    ctx.provider = "copilot";
    ctx.error_class = "AuthError";
    ctx.message = "token expired";
    const std::string s {rp::format_runtime_provider_error(ctx)};
    EXPECT_NE(s.find("Authentication failed"), std::string::npos);
    EXPECT_NE(s.find("copilot"), std::string::npos);
    EXPECT_NE(s.find("token expired"), std::string::npos);
}

TEST(RuntimeProviderFormatErr, ValueError) {
    rp::ErrorContext ctx {};
    ctx.error_class = "ValueError";
    ctx.message = "missing field";
    EXPECT_NE(rp::format_runtime_provider_error(ctx).find("Invalid runtime"),
              std::string::npos);
}

TEST(RuntimeProviderFormatErr, AnsiStripped) {
    rp::ErrorContext ctx {};
    ctx.provider = "nous";
    ctx.error_class = "AuthError";
    ctx.message = "\x1b[31merror\x1b[0m message";
    const std::string s {rp::format_runtime_provider_error(ctx)};
    EXPECT_EQ(s.find("\x1b"), std::string::npos);
    EXPECT_NE(s.find("error message"), std::string::npos);
}

TEST(RuntimeProviderFormatErr, GenericFallback) {
    rp::ErrorContext ctx {};
    ctx.provider = "nous";
    ctx.error_class = "RuntimeError";
    ctx.message = "pool drained";
    const std::string s {rp::format_runtime_provider_error(ctx)};
    EXPECT_NE(s.find("nous"), std::string::npos);
    EXPECT_NE(s.find("pool drained"), std::string::npos);
}

// ---------------------------------------------------------------------------
// resolve_runtime_from_pool_entry
// ---------------------------------------------------------------------------

TEST(RuntimeProviderResolveEntry, CodexAppliesDefaults) {
    rp::PoolEntry entry {};
    entry.access_token = "TOK";
    entry.source = "oauth";
    const rp::ModelConfig cfg {};
    const auto out {rp::resolve_runtime_from_pool_entry(
        "openai-codex", entry, "auto", cfg)};
    EXPECT_EQ(out.provider, "openai-codex");
    EXPECT_EQ(out.api_mode, "codex_responses");
    EXPECT_EQ(out.base_url, rp::kDefaultCodexBaseUrl);
    EXPECT_EQ(out.api_key, "TOK");
    EXPECT_EQ(out.source, "oauth");
    EXPECT_EQ(out.requested_provider, "auto");
}

TEST(RuntimeProviderResolveEntry, AnthropicUsesConfigBaseUrl) {
    rp::PoolEntry entry {};
    entry.runtime_api_key = "KEY";
    rp::ModelConfig cfg {};
    cfg.provider = "anthropic";
    cfg.base_url = "https://my-proxy/anthropic/";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "anthropic", entry, "anthropic", cfg)};
    EXPECT_EQ(out.api_mode, "anthropic_messages");
    EXPECT_EQ(out.base_url, "https://my-proxy/anthropic");
}

TEST(RuntimeProviderResolveEntry, AnthropicDefaultsWhenNoConfig) {
    rp::PoolEntry entry {};
    entry.runtime_api_key = "K";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "anthropic", entry, "anthropic", {})};
    EXPECT_EQ(out.base_url, rp::kDefaultAnthropicBaseUrl);
}

TEST(RuntimeProviderResolveEntry, OpenRouterDefault) {
    rp::PoolEntry entry {};
    entry.runtime_api_key = "K";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "openrouter", entry, "openrouter", {})};
    EXPECT_EQ(out.base_url, rp::kDefaultOpenRouterBaseUrl);
    EXPECT_EQ(out.api_mode, "chat_completions");
}

TEST(RuntimeProviderResolveEntry, UrlSuffixTriggersAnthropic) {
    rp::PoolEntry entry {};
    entry.runtime_base_url = "https://opencode.ai/zen/go/anthropic";
    entry.runtime_api_key = "K";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "opencode-zen", entry, "opencode-zen", {})};
    EXPECT_EQ(out.api_mode, "anthropic_messages");
}

TEST(RuntimeProviderResolveEntry, OpenCodeV1Stripped) {
    rp::PoolEntry entry {};
    entry.runtime_base_url = "https://opencode.ai/zen/v1";
    entry.runtime_api_key = "K";
    rp::ModelConfig cfg {};
    cfg.provider = "opencode-zen";
    cfg.api_mode = "anthropic_messages";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "opencode-zen", entry, "opencode-zen", cfg)};
    EXPECT_EQ(out.base_url, "https://opencode.ai/zen");
}

TEST(RuntimeProviderResolveEntry, StaleApiModeIgnored) {
    // configured_provider=openai, runtime=anthropic — api_mode
    // from config must not leak.
    rp::PoolEntry entry {};
    entry.runtime_api_key = "K";
    rp::ModelConfig cfg {};
    cfg.provider = "openai-codex";
    cfg.api_mode = "codex_responses";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "nous", entry, "nous", cfg)};
    EXPECT_EQ(out.api_mode, "chat_completions");
}

TEST(RuntimeProviderResolveEntry, PrefersRuntimeOverBase) {
    rp::PoolEntry entry {};
    entry.runtime_base_url = "https://runtime";
    entry.base_url = "https://fallback";
    entry.runtime_api_key = "rk";
    entry.access_token = "tok";
    const auto out {rp::resolve_runtime_from_pool_entry(
        "nous", entry, "nous", {})};
    EXPECT_EQ(out.base_url, "https://runtime");
    EXPECT_EQ(out.api_key, "rk");
}

// ---------------------------------------------------------------------------
// extract_custom_provider_name / pool-backed check.
// ---------------------------------------------------------------------------

TEST(RuntimeProviderCustomName, Extracts) {
    EXPECT_EQ(rp::extract_custom_provider_name("custom:acme"), "acme");
    EXPECT_EQ(rp::extract_custom_provider_name("CUSTOM:Foo"), "foo");
}

TEST(RuntimeProviderCustomName, NonMatching) {
    EXPECT_EQ(rp::extract_custom_provider_name("anthropic"), "");
    EXPECT_EQ(rp::extract_custom_provider_name("custom"), "");
}

TEST(RuntimeProviderPool, IsPoolBacked) {
    EXPECT_TRUE(rp::is_pool_backed_provider("anthropic"));
    EXPECT_TRUE(rp::is_pool_backed_provider("copilot"));
    EXPECT_FALSE(rp::is_pool_backed_provider("custom"));
    EXPECT_FALSE(rp::is_pool_backed_provider("mystery"));
}
