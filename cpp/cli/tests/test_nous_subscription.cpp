// Tests for the C++17 port of `hermes_cli/nous_subscription.py`.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "hermes/cli/nous_subscription.hpp"

using namespace hermes::cli::nous_subscription;
using json = nlohmann::json;

TEST(NousSubscription, ModelConfigDictObjectPassThrough) {
    json cfg{{"model", {{"provider", "nous"}, {"default", "claude"}}}};
    auto result = model_config_dict(cfg);
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result.at("provider").get<std::string>(), "nous");
    EXPECT_EQ(result.at("default").get<std::string>(), "claude");
}

TEST(NousSubscription, ModelConfigDictStringWrapped) {
    json cfg{{"model", "claude-sonnet"}};
    auto result = model_config_dict(cfg);
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result.at("default").get<std::string>(), "claude-sonnet");
}

TEST(NousSubscription, ModelConfigDictStringStripsWhitespace) {
    json cfg{{"model", "   claude   "}};
    auto result = model_config_dict(cfg);
    EXPECT_EQ(result.at("default").get<std::string>(), "claude");
}

TEST(NousSubscription, ModelConfigDictEmptyStringReturnsEmpty) {
    json cfg{{"model", "   "}};
    auto result = model_config_dict(cfg);
    EXPECT_TRUE(result.is_object());
    EXPECT_TRUE(result.empty());
}

TEST(NousSubscription, ModelConfigDictMissingKey) {
    json cfg{{"other", 1}};
    auto result = model_config_dict(cfg);
    EXPECT_TRUE(result.is_object());
    EXPECT_TRUE(result.empty());
}

TEST(NousSubscription, ModelConfigDictNonObjectInput) {
    json cfg = "not-an-object";
    auto result = model_config_dict(cfg);
    EXPECT_TRUE(result.is_object());
    EXPECT_TRUE(result.empty());
}

TEST(NousSubscription, BrowserLabelKnownProviders) {
    EXPECT_EQ(browser_label("browserbase"), "Browserbase");
    EXPECT_EQ(browser_label("browser-use"), "Browser Use");
    EXPECT_EQ(browser_label("firecrawl"), "Firecrawl");
    EXPECT_EQ(browser_label("camofox"), "Camofox");
    EXPECT_EQ(browser_label("local"), "Local browser");
}

TEST(NousSubscription, BrowserLabelDefaultsToLocalBrowser) {
    EXPECT_EQ(browser_label(""), "Local browser");
}

TEST(NousSubscription, BrowserLabelUnknownReturnsInput) {
    EXPECT_EQ(browser_label("mystery"), "mystery");
}

TEST(NousSubscription, TtsLabelKnownProviders) {
    EXPECT_EQ(tts_label("openai"), "OpenAI TTS");
    EXPECT_EQ(tts_label("elevenlabs"), "ElevenLabs");
    EXPECT_EQ(tts_label("edge"), "Edge TTS");
    EXPECT_EQ(tts_label("mistral"), "Mistral Voxtral TTS");
    EXPECT_EQ(tts_label("neutts"), "NeuTTS");
}

TEST(NousSubscription, TtsLabelDefaultsToEdgeTTS) {
    EXPECT_EQ(tts_label(""), "Edge TTS");
}

TEST(NousSubscription, TtsLabelUnknownReturnsInput) {
    EXPECT_EQ(tts_label("future-tts"), "future-tts");
}

TEST(NousSubscription, ResolveBrowserCamofoxDirectWins) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.direct_camofox = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "camofox");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_FALSE(out.managed);
}

TEST(NousSubscription, ResolveBrowserExplicitBrowserbase) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "browserbase";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    in.direct_browserbase = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "browserbase");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_FALSE(out.managed);
}

TEST(NousSubscription, ResolveBrowserExplicitBrowserbaseMissingKey) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "browserbase";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    in.direct_browserbase = false;
    auto out = resolve_browser_feature_state(in);
    EXPECT_FALSE(out.available);
    EXPECT_FALSE(out.active);
}

TEST(NousSubscription, ResolveBrowserExplicitBrowserUseManaged) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "browser-use";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    in.managed_browser_available = true;
    in.direct_browser_use = false;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "browser-use");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_TRUE(out.managed);
}

TEST(NousSubscription, ResolveBrowserExplicitBrowserUseDirectKeyWins) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "browser-use";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    in.managed_browser_available = true;
    in.direct_browser_use = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_FALSE(out.managed);
}

TEST(NousSubscription, ResolveBrowserExplicitFirecrawl) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "firecrawl";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    in.direct_firecrawl = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "firecrawl");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_FALSE(out.managed);
}

TEST(NousSubscription, ResolveBrowserExplicitCamofoxNoKeyYieldsUnavailable) {
    browser_feature_inputs in{};
    in.browser_provider = "camofox";
    in.browser_provider_explicit = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "camofox");
    EXPECT_FALSE(out.available);
    EXPECT_FALSE(out.active);
}

TEST(NousSubscription, ResolveBrowserExplicitUnknownFallsBackToLocal) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_provider = "unknown-provider";
    in.browser_provider_explicit = true;
    in.browser_local_available = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "local");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
}

TEST(NousSubscription, ResolveBrowserImplicitManagedBrowserUse) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_local_available = true;
    in.managed_browser_available = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "browser-use");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
    EXPECT_TRUE(out.managed);
}

TEST(NousSubscription, ResolveBrowserImplicitBrowserbase) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_local_available = true;
    in.direct_browserbase = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "browserbase");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
}

TEST(NousSubscription, ResolveBrowserImplicitLocalOnly) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    in.browser_local_available = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "local");
    EXPECT_TRUE(out.available);
    EXPECT_TRUE(out.active);
}

TEST(NousSubscription, ResolveBrowserLocalBinaryMissing) {
    browser_feature_inputs in{};
    in.browser_tool_enabled = true;
    auto out = resolve_browser_feature_state(in);
    EXPECT_EQ(out.current_provider, "local");
    EXPECT_FALSE(out.available);
    EXPECT_FALSE(out.active);
}

TEST(NousSubscription, ExplainerEmptyWhenManagedDisabled) {
    auto lines = nous_subscription_explainer_lines(false);
    EXPECT_TRUE(lines.empty());
}

TEST(NousSubscription, ExplainerReturnsThreeLines) {
    auto lines = nous_subscription_explainer_lines(true);
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("managed web tools"), std::string::npos);
    EXPECT_NE(lines[1].find("bill to your Nous subscription"), std::string::npos);
    EXPECT_NE(lines[2].find("hermes setup"), std::string::npos);
}

TEST(NousSubscription, ProviderDefaultsDisabledReturnsEmpty) {
    auto dec = apply_nous_provider_defaults_decision(false, true, "");
    EXPECT_TRUE(dec.to_change.empty());
    EXPECT_FALSE(dec.set_tts_provider_openai);
}

TEST(NousSubscription, ProviderDefaultsNotNousReturnsEmpty) {
    auto dec = apply_nous_provider_defaults_decision(true, false, "");
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ProviderDefaultsEdgeSwitchesToOpenAI) {
    auto dec = apply_nous_provider_defaults_decision(true, true, "edge");
    EXPECT_TRUE(dec.set_tts_provider_openai);
    EXPECT_EQ(dec.to_change.count("tts"), 1u);
}

TEST(NousSubscription, ProviderDefaultsEmptySwitchesToOpenAI) {
    auto dec = apply_nous_provider_defaults_decision(true, true, "");
    EXPECT_TRUE(dec.set_tts_provider_openai);
}

TEST(NousSubscription, ProviderDefaultsRespectsExistingOverride) {
    auto dec = apply_nous_provider_defaults_decision(true, true, "elevenlabs");
    EXPECT_FALSE(dec.set_tts_provider_openai);
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ManagedDefaultsDisabledReturnsEmpty) {
    managed_defaults_inputs in{};
    in.provider_is_nous = true;
    in.selected_toolsets = {"web", "tts", "browser", "image_gen"};
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ManagedDefaultsRequiresNousProvider) {
    managed_defaults_inputs in{};
    in.managed_enabled = true;
    in.provider_is_nous = false;
    in.selected_toolsets = {"web", "tts", "browser"};
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ManagedDefaultsSetsExpectedValues) {
    managed_defaults_inputs in{};
    in.managed_enabled = true;
    in.provider_is_nous = true;
    in.selected_toolsets = {"web", "tts", "browser", "image_gen"};
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_TRUE(dec.set_web_backend_firecrawl);
    EXPECT_TRUE(dec.set_tts_provider_openai);
    EXPECT_TRUE(dec.set_browser_cloud_provider_browser_use);
    EXPECT_EQ(dec.to_change.count("web"), 1u);
    EXPECT_EQ(dec.to_change.count("tts"), 1u);
    EXPECT_EQ(dec.to_change.count("browser"), 1u);
    EXPECT_EQ(dec.to_change.count("image_gen"), 1u);
}

TEST(NousSubscription, ManagedDefaultsSkipsExplicitlyConfigured) {
    managed_defaults_inputs in{};
    in.managed_enabled = true;
    in.provider_is_nous = true;
    in.selected_toolsets = {"web", "tts", "browser"};
    in.web_explicit_configured = true;
    in.tts_explicit_configured = true;
    in.browser_explicit_configured = true;
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_FALSE(dec.set_web_backend_firecrawl);
    EXPECT_FALSE(dec.set_tts_provider_openai);
    EXPECT_FALSE(dec.set_browser_cloud_provider_browser_use);
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ManagedDefaultsSkipsWhenDirectEnvPresent) {
    managed_defaults_inputs in{};
    in.managed_enabled = true;
    in.provider_is_nous = true;
    in.selected_toolsets = {"web", "tts", "browser", "image_gen"};
    in.has_parallel_or_tavily_or_firecrawl_env = true;
    in.has_openai_audio_or_elevenlabs_env = true;
    in.has_browser_use_or_browserbase_env = true;
    in.has_fal_env = true;
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_FALSE(dec.set_web_backend_firecrawl);
    EXPECT_FALSE(dec.set_tts_provider_openai);
    EXPECT_FALSE(dec.set_browser_cloud_provider_browser_use);
    EXPECT_TRUE(dec.to_change.empty());
}

TEST(NousSubscription, ManagedDefaultsOnlyActsOnSelectedToolsets) {
    managed_defaults_inputs in{};
    in.managed_enabled = true;
    in.provider_is_nous = true;
    in.selected_toolsets = {"web"};  // only web is selected
    auto dec = apply_nous_managed_defaults_decision(in);
    EXPECT_TRUE(dec.set_web_backend_firecrawl);
    EXPECT_FALSE(dec.set_tts_provider_openai);
    EXPECT_FALSE(dec.set_browser_cloud_provider_browser_use);
    EXPECT_EQ(dec.to_change.count("web"), 1u);
    EXPECT_EQ(dec.to_change.count("tts"), 0u);
}

TEST(NousSubscription, FeatureStateAtThrowsOnMissing) {
    nous_subscription_features features{};
    EXPECT_THROW(features.at("missing"), std::out_of_range);
}

TEST(NousSubscription, FeatureStateOrderedItems) {
    nous_subscription_features features{};
    features.features.emplace("web", nous_feature_state{});
    features.features.emplace("modal", nous_feature_state{});
    features.features.emplace("browser", nous_feature_state{});
    features.features.emplace("tts", nous_feature_state{});
    features.features.emplace("image_gen", nous_feature_state{});
    auto items = features.ordered_items();
    ASSERT_EQ(items.size(), 5u);
    // Order must match k_feature_order regardless of insertion order.
    EXPECT_EQ(items[0].key, "");  // default-constructed structs have empty key
    EXPECT_EQ(items.size(), 5u);
}

TEST(NousSubscription, FeatureOrderConstant) {
    ASSERT_EQ(k_feature_order.size(), 5u);
    EXPECT_STREQ(k_feature_order[0], "web");
    EXPECT_STREQ(k_feature_order[1], "image_gen");
    EXPECT_STREQ(k_feature_order[2], "tts");
    EXPECT_STREQ(k_feature_order[3], "browser");
    EXPECT_STREQ(k_feature_order[4], "modal");
}
