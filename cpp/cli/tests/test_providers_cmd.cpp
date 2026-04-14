// Tests for cpp/cli/src/providers_cmd.cpp — overlays, alias folding,
// ProviderDef resolution, API-mode heuristics, custom-provider slugging.
#include "hermes/cli/providers_cmd.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace pc = hermes::cli::providers_cmd;

TEST(ProvidersCmd, NormalizeFoldsAliases) {
    EXPECT_EQ(pc::normalize_provider("  CLAUDE "), "anthropic");
    EXPECT_EQ(pc::normalize_provider("github"), "github-copilot");
    EXPECT_EQ(pc::normalize_provider("openai"), "openrouter");
    EXPECT_EQ(pc::normalize_provider("z.ai"), "zai");
    EXPECT_EQ(pc::normalize_provider("deep-seek"), "deepseek");
}

TEST(ProvidersCmd, NormalizePassesThroughCanonical) {
    EXPECT_EQ(pc::normalize_provider("anthropic"), "anthropic");
    EXPECT_EQ(pc::normalize_provider("nous"), "nous");
    EXPECT_EQ(pc::normalize_provider("mystery"), "mystery");
    EXPECT_EQ(pc::normalize_provider(""), "");
}

TEST(ProvidersCmd, GetProviderLooksUpOverlay) {
    auto p = pc::get_provider("anthropic");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->id, "anthropic");
    EXPECT_EQ(p->transport, "anthropic_messages");
    EXPECT_EQ(p->source, "hermes");
}

TEST(ProvidersCmd, GetProviderUnknownReturnsNullopt) {
    EXPECT_FALSE(pc::get_provider("no-such-provider").has_value());
}

TEST(ProvidersCmd, GetLabelOverrides) {
    EXPECT_EQ(pc::get_label("nous"), "Nous Portal");
    EXPECT_EQ(pc::get_label("openai-codex"), "OpenAI Codex");
    EXPECT_EQ(pc::get_label("copilot-acp"), "GitHub Copilot ACP");
    EXPECT_EQ(pc::get_label("mystery"), "mystery");
}

TEST(ProvidersCmd, IsAggregator) {
    EXPECT_TRUE(pc::is_aggregator("openrouter"));
    EXPECT_TRUE(pc::is_aggregator("vercel"));
    EXPECT_FALSE(pc::is_aggregator("anthropic"));
    EXPECT_FALSE(pc::is_aggregator("nous"));
}

TEST(ProvidersCmd, DetermineApiMode) {
    EXPECT_EQ(pc::determine_api_mode("anthropic"), "anthropic_messages");
    EXPECT_EQ(pc::determine_api_mode("openai-codex"), "codex_responses");
    EXPECT_EQ(pc::determine_api_mode("openrouter"), "chat_completions");
    // URL heuristics for unknowns.
    EXPECT_EQ(pc::determine_api_mode("mystery", "https://api.anthropic.com/v1"),
              "anthropic_messages");
    EXPECT_EQ(pc::determine_api_mode("mystery", "https://api.openai.com/v1"),
              "codex_responses");
    EXPECT_EQ(pc::determine_api_mode("mystery", "https://anything/anthropic"),
              "anthropic_messages");
    EXPECT_EQ(pc::determine_api_mode("mystery", "https://foo.bar/v1"),
              "chat_completions");
}

TEST(ProvidersCmd, CustomProviderSlug) {
    EXPECT_EQ(pc::custom_provider_slug("My Ollama"), "custom:my-ollama");
    EXPECT_EQ(pc::custom_provider_slug("  Local Server  "), "custom:local-server");
    EXPECT_EQ(pc::custom_provider_slug("ALL-CAPS"), "custom:all-caps");
}

TEST(ProvidersCmd, ResolveUserProviderPicksFields) {
    nlohmann::json cfg = {
        {"mycorp", {
            {"name", "My Corp LLM"},
            {"api", "https://llm.corp/v1"},
            {"key_env", "MYCORP_API_KEY"},
            {"transport", "openai_chat"},
        }}
    };
    auto p = pc::resolve_user_provider("mycorp", cfg);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->name, "My Corp LLM");
    EXPECT_EQ(p->base_url, "https://llm.corp/v1");
    ASSERT_EQ(p->api_key_env_vars.size(), 1u);
    EXPECT_EQ(p->api_key_env_vars[0], "MYCORP_API_KEY");
    EXPECT_EQ(p->source, "user-config");
}

TEST(ProvidersCmd, ResolveUserProviderMissingReturnsNullopt) {
    nlohmann::json empty = nlohmann::json::object();
    EXPECT_FALSE(pc::resolve_user_provider("nope", empty).has_value());
    EXPECT_FALSE(pc::resolve_user_provider("nope", nullptr).has_value());
}

TEST(ProvidersCmd, ResolveCustomProviderBySlugAndDisplayName) {
    nlohmann::json arr = nlohmann::json::array({
        {
            {"name", "My Ollama"},
            {"base_url", "http://localhost:11434/v1"},
        }
    });
    auto by_display = pc::resolve_custom_provider("my ollama", arr);
    ASSERT_TRUE(by_display.has_value());
    EXPECT_EQ(by_display->id, "custom:my-ollama");

    auto by_slug = pc::resolve_custom_provider("custom:my-ollama", arr);
    ASSERT_TRUE(by_slug.has_value());
    EXPECT_EQ(by_slug->name, "My Ollama");

    auto none = pc::resolve_custom_provider("who", arr);
    EXPECT_FALSE(none.has_value());
}

TEST(ProvidersCmd, ResolveProviderFullFallsThroughToCustom) {
    nlohmann::json user_providers = nlohmann::json::object();
    nlohmann::json custom = nlohmann::json::array({
        {{"name", "Lab"}, {"url", "http://lab/v1"}}
    });
    auto p = pc::resolve_provider_full("lab", user_providers, custom);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->id, "custom:lab");
}

TEST(ProvidersCmd, ResolveProviderFullPrefersBuiltin) {
    nlohmann::json user_providers = {
        {"anthropic", {{"name", "Hijacked"}, {"api", "http://nope"}}}
    };
    auto p = pc::resolve_provider_full("anthropic", user_providers, nullptr);
    ASSERT_TRUE(p.has_value());
    // Built-in overlay wins — display name comes from label overrides.
    EXPECT_EQ(p->source, "hermes");
}

TEST(ProvidersCmd, OverlayTableCompleteness) {
    // Spot-check the overlays the CLI cares about exist.
    const auto& t = pc::hermes_overlays();
    for (const auto* id : {"openrouter", "anthropic", "nous", "openai-codex",
                           "qwen-oauth", "copilot-acp", "github-copilot",
                           "zai", "xai", "deepseek", "alibaba", "huggingface"}) {
        EXPECT_TRUE(t.count(id) == 1) << "missing overlay for " << id;
    }
}

TEST(ProvidersCmd, ListOverlayProvidersSorted) {
    auto v = pc::list_overlay_providers();
    EXPECT_FALSE(v.empty());
    EXPECT_TRUE(std::is_sorted(v.begin(), v.end()));
}

TEST(ProvidersCmd, ApiKeyEnvVarsForKnownAndUnknown) {
    auto anth = pc::api_key_env_vars_for("anthropic");
    EXPECT_FALSE(anth.empty());
    auto none = pc::api_key_env_vars_for("mystery");
    EXPECT_TRUE(none.empty());
}

TEST(ProvidersCmd, TransportToApiModeCoversKnownTransports) {
    const auto& m = pc::transport_to_api_mode();
    EXPECT_EQ(m.at("openai_chat"), "chat_completions");
    EXPECT_EQ(m.at("anthropic_messages"), "anthropic_messages");
    EXPECT_EQ(m.at("codex_responses"), "codex_responses");
}
