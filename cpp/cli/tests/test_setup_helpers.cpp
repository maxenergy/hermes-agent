// Tests for hermes::cli::setup_helpers (port of hermes_cli/setup.py
// pure-logic primitives).
#include "hermes/cli/setup_helpers.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::setup_helpers;
using nlohmann::json;

// ---------------------------------------------------------------------------
// Default model snapshots.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_DefaultModels, KnownProviders) {
    EXPECT_FALSE(default_models_for_provider("copilot").empty());
    EXPECT_FALSE(default_models_for_provider("gemini").empty());
    EXPECT_FALSE(default_models_for_provider("zai").empty());
    EXPECT_FALSE(default_models_for_provider("huggingface").empty());
    EXPECT_TRUE(has_default_models("copilot"));
    EXPECT_TRUE(has_default_models("kimi-coding"));
}

TEST(SetupHelpers_DefaultModels, UnknownProviderEmpty) {
    EXPECT_TRUE(default_models_for_provider("does-not-exist").empty());
    EXPECT_FALSE(has_default_models("does-not-exist"));
    EXPECT_FALSE(has_default_models(""));
}

TEST(SetupHelpers_DefaultModels, CopilotEntriesMatchPython) {
    const std::string pid = "copilot";
    const auto& copilot = default_models_for_provider(pid);
    // Sanity-check the canonical IDs.
    EXPECT_EQ(copilot.front(), "gpt-5.4");
    EXPECT_NE(std::find(copilot.begin(), copilot.end(),
                        std::string("claude-opus-4.6")),
              copilot.end());
    EXPECT_NE(std::find(copilot.begin(), copilot.end(),
                        std::string("grok-code-fast-1")),
              copilot.end());
}

TEST(SetupHelpers_DefaultModels, ProvidersWithDefaultsSorted) {
    auto ids = providers_with_default_models();
    EXPECT_FALSE(ids.empty());
    for (size_t i = 1; i < ids.size(); ++i) {
        EXPECT_LT(ids[i - 1], ids[i]);
    }
}

// ---------------------------------------------------------------------------
// Model config dict / set_default_model.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_ModelDict, EmptyConfigYieldsObject) {
    json cfg = json::object();
    auto m = model_config_dict(cfg);
    EXPECT_TRUE(m.is_object());
    EXPECT_TRUE(m.empty());
}

TEST(SetupHelpers_ModelDict, StringHoistedToDefault) {
    json cfg = {{"model", "gpt-5.4"}};
    auto m = model_config_dict(cfg);
    EXPECT_EQ(m["default"], "gpt-5.4");
}

TEST(SetupHelpers_ModelDict, ObjectPreserved) {
    json cfg = {{"model", {{"default", "abc"}, {"base_url", "http://x"}}}};
    auto m = model_config_dict(cfg);
    EXPECT_EQ(m["default"], "abc");
    EXPECT_EQ(m["base_url"], "http://x");
}

TEST(SetupHelpers_ModelDict, SetDefaultModelOnEmpty) {
    json cfg = json::object();
    set_default_model(cfg, "claude-opus-4.6");
    EXPECT_EQ(cfg["model"]["default"], "claude-opus-4.6");
}

TEST(SetupHelpers_ModelDict, SetDefaultModelEmptyNameIsNoOp) {
    json cfg = json::object();
    set_default_model(cfg, "");
    EXPECT_TRUE(cfg.find("model") == cfg.end());
}

TEST(SetupHelpers_ModelDict, SetDefaultPreservesOtherKeys) {
    json cfg = {{"model", {{"base_url", "http://x"}, {"default", "old"}}}};
    set_default_model(cfg, "new");
    EXPECT_EQ(cfg["model"]["default"], "new");
    EXPECT_EQ(cfg["model"]["base_url"], "http://x");
}

// ---------------------------------------------------------------------------
// Credential pool strategies.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_CredPool, GetEmpty) {
    json cfg = json::object();
    EXPECT_TRUE(get_credential_pool_strategies(cfg).empty());
}

TEST(SetupHelpers_CredPool, SetThenGet) {
    json cfg = json::object();
    set_credential_pool_strategy(cfg, "openrouter", "round_robin");
    set_credential_pool_strategy(cfg, "anthropic", "first");
    auto m = get_credential_pool_strategies(cfg);
    EXPECT_EQ(m["openrouter"], "round_robin");
    EXPECT_EQ(m["anthropic"], "first");
}

TEST(SetupHelpers_CredPool, EmptyProviderIsNoOp) {
    json cfg = json::object();
    set_credential_pool_strategy(cfg, "", "any");
    EXPECT_TRUE(get_credential_pool_strategies(cfg).empty());
}

TEST(SetupHelpers_CredPool, SupportsSameProviderPool) {
    auto lookup = [](const std::string& p) -> std::string {
        if (p == "openai") return "api_key";
        if (p == "github") return "oauth_device_code";
        if (p == "weird") return "oauth";  // not eligible
        return {};
    };
    EXPECT_TRUE(supports_same_provider_pool_setup("openrouter", lookup));
    EXPECT_TRUE(supports_same_provider_pool_setup("openai", lookup));
    EXPECT_TRUE(supports_same_provider_pool_setup("github", lookup));
    EXPECT_FALSE(supports_same_provider_pool_setup("weird", lookup));
    EXPECT_FALSE(supports_same_provider_pool_setup("custom", lookup));
    EXPECT_FALSE(supports_same_provider_pool_setup("", lookup));
    EXPECT_FALSE(supports_same_provider_pool_setup("unknown", lookup));
}

// ---------------------------------------------------------------------------
// Reasoning effort.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_Reasoning, CurrentEmpty) {
    json cfg = json::object();
    EXPECT_EQ(current_reasoning_effort(cfg), "");
}

TEST(SetupHelpers_Reasoning, CurrentNormalised) {
    json cfg = {{"agent", {{"reasoning_effort", "  Medium  "}}}};
    EXPECT_EQ(current_reasoning_effort(cfg), "medium");
}

TEST(SetupHelpers_Reasoning, SetCreatesAgentSubobject) {
    json cfg = json::object();
    set_reasoning_effort(cfg, "low");
    EXPECT_EQ(cfg["agent"]["reasoning_effort"], "low");
}

TEST(SetupHelpers_Reasoning, SetPreservesAgentKeys) {
    json cfg = {{"agent", {{"max_turns", 50}}}};
    set_reasoning_effort(cfg, "high");
    EXPECT_EQ(cfg["agent"]["reasoning_effort"], "high");
    EXPECT_EQ(cfg["agent"]["max_turns"], 50);
}

TEST(SetupHelpers_Reasoning, ChoiceMenuDefaultsToCurrent) {
    json cfg = {{"agent", {{"reasoning_effort", "low"}}}};
    auto menu = build_reasoning_choice_menu(cfg, {"low", "medium", "high"});
    ASSERT_EQ(menu.choices.size(), 5u);  // 3 efforts + Disable + Keep
    EXPECT_EQ(menu.choices[0], "low");
    EXPECT_EQ(menu.choices[3], "Disable reasoning");
    EXPECT_EQ(menu.choices[4], std::string("Keep current (low)"));
    EXPECT_EQ(menu.default_index, 0);
}

TEST(SetupHelpers_Reasoning, ChoiceMenuPrefersMediumWhenUnknown) {
    json cfg = json::object();
    auto menu = build_reasoning_choice_menu(cfg, {"low", "medium", "high"});
    EXPECT_EQ(menu.default_index, 1);  // medium
    EXPECT_EQ(menu.choices.back(), "Keep current (default)");
}

TEST(SetupHelpers_Reasoning, ChoiceMenuFallsBackToKeepWhenNoMedium) {
    json cfg = json::object();
    auto menu = build_reasoning_choice_menu(cfg, {"low", "high"});
    EXPECT_EQ(menu.default_index, static_cast<int>(menu.choices.size()) - 1);
}

TEST(SetupHelpers_Reasoning, ChoiceMenuNoneSelectsDisable) {
    json cfg = {{"agent", {{"reasoning_effort", "none"}}}};
    auto menu = build_reasoning_choice_menu(cfg, {"low", "medium", "high"});
    EXPECT_EQ(menu.default_index, 3);
    EXPECT_EQ(menu.choices[3], "Disable reasoning");
}

TEST(SetupHelpers_Reasoning, ApplyChoiceWithinEffortsSetsEffort) {
    json cfg = json::object();
    apply_reasoning_choice(cfg, {"low", "medium", "high"}, 2);
    EXPECT_EQ(cfg["agent"]["reasoning_effort"], "high");
}

TEST(SetupHelpers_Reasoning, ApplyChoiceDisableSetsNone) {
    json cfg = json::object();
    apply_reasoning_choice(cfg, {"low", "medium", "high"}, 3);
    EXPECT_EQ(cfg["agent"]["reasoning_effort"], "none");
}

TEST(SetupHelpers_Reasoning, ApplyChoiceKeepIsNoOp) {
    json cfg = json::object();
    // efforts.size()==2 → idx 0,1 are efforts; idx 2 is Disable; idx 3 is Keep.
    apply_reasoning_choice(cfg, {"low", "medium"}, 3);
    EXPECT_TRUE(cfg.find("agent") == cfg.end());
}

TEST(SetupHelpers_Reasoning, ApplyChoiceNegativeIgnored) {
    json cfg = json::object();
    apply_reasoning_choice(cfg, {"low", "high"}, -1);
    EXPECT_TRUE(cfg.find("agent") == cfg.end());
}

// ---------------------------------------------------------------------------
// Section summary.
// ---------------------------------------------------------------------------

namespace {
EnvLookupFn empty_env() {
    return [](const std::string&) { return std::string{}; };
}
}  // namespace

TEST(SetupHelpers_Sections, ModelNoCredsReturnsNullopt) {
    json cfg = json::object();
    auto s = get_section_config_summary(cfg, "model", empty_env(), {});
    EXPECT_FALSE(s.has_value());
}

TEST(SetupHelpers_Sections, ModelEnvKeyTriggersConfigured) {
    json cfg = json::object();
    auto env = [](const std::string& n) -> std::string {
        return n == "OPENROUTER_API_KEY" ? "sk-x" : "";
    };
    auto s = get_section_config_summary(cfg, "model", env, {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "configured");
}

TEST(SetupHelpers_Sections, ModelDictWithDefault) {
    json cfg = {{"model", {{"default", "gpt-5"}}}};
    auto env = [](const std::string& n) -> std::string {
        return n == "ANTHROPIC_API_KEY" ? "x" : "";
    };
    auto s = get_section_config_summary(cfg, "model", env, {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "gpt-5");
}

TEST(SetupHelpers_Sections, ModelStringValue) {
    json cfg = {{"model", " gpt-5.4 "}};
    auto env = [](const std::string& n) -> std::string {
        return n == "OPENAI_API_KEY" ? "x" : "";
    };
    auto s = get_section_config_summary(cfg, "model", env, {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "gpt-5.4");
}

TEST(SetupHelpers_Sections, ModelActiveProviderTriggersConfigured) {
    json cfg = json::object();
    auto active = []() -> std::optional<std::string> {
        return std::string("openai-codex");
    };
    auto s = get_section_config_summary(cfg, "model", empty_env(), active);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "configured");
}

TEST(SetupHelpers_Sections, TerminalDefaultBackend) {
    json cfg = json::object();
    auto s = get_section_config_summary(cfg, "terminal", empty_env(), {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "backend: local");
}

TEST(SetupHelpers_Sections, TerminalCustomBackend) {
    json cfg = {{"terminal", {{"backend", "docker"}}}};
    auto s = get_section_config_summary(cfg, "terminal", empty_env(), {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "backend: docker");
}

TEST(SetupHelpers_Sections, AgentDefaultMaxTurns) {
    json cfg = json::object();
    auto s = get_section_config_summary(cfg, "agent", empty_env(), {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "max turns: 90");
}

TEST(SetupHelpers_Sections, AgentCustomMaxTurns) {
    json cfg = {{"agent", {{"max_turns", 200}}}};
    auto s = get_section_config_summary(cfg, "agent", empty_env(), {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "max turns: 200");
}

TEST(SetupHelpers_Sections, GatewayNoPlatforms) {
    json cfg = json::object();
    auto s = get_section_config_summary(cfg, "gateway", empty_env(), {});
    EXPECT_FALSE(s.has_value());
}

TEST(SetupHelpers_Sections, GatewayMultiPlatforms) {
    json cfg = json::object();
    auto env = [](const std::string& n) -> std::string {
        if (n == "TELEGRAM_BOT_TOKEN") return "x";
        if (n == "DISCORD_BOT_TOKEN") return "x";
        if (n == "BLUEBUBBLES_SERVER_URL") return "http://x";
        return "";
    };
    auto s = get_section_config_summary(cfg, "gateway", env, {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "Telegram, Discord, BlueBubbles");
}

TEST(SetupHelpers_Sections, ToolsConfigured) {
    json cfg = json::object();
    auto env = [](const std::string& n) -> std::string {
        if (n == "ELEVENLABS_API_KEY") return "x";
        if (n == "FIRECRAWL_API_KEY") return "x";
        return "";
    };
    auto s = get_section_config_summary(cfg, "tools", env, {});
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, "TTS/ElevenLabs, Firecrawl");
}

TEST(SetupHelpers_Sections, UnknownSectionNullopt) {
    json cfg = json::object();
    auto s = get_section_config_summary(cfg, "totally-unknown", empty_env(), {});
    EXPECT_FALSE(s.has_value());
}

// ---------------------------------------------------------------------------
// Discord user-id cleanup.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_Discord, EmptyInput) {
    auto ids = clean_discord_user_ids("");
    EXPECT_TRUE(ids.empty());
}

TEST(SetupHelpers_Discord, PlainCommaSeparated) {
    auto ids = clean_discord_user_ids("123, 456, 789");
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], "123");
    EXPECT_EQ(ids[1], "456");
    EXPECT_EQ(ids[2], "789");
}

TEST(SetupHelpers_Discord, StripMentionPrefix) {
    auto ids = clean_discord_user_ids("<@123>, <@!456>");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "123");
    EXPECT_EQ(ids[1], "456");
}

TEST(SetupHelpers_Discord, StripUserPrefix) {
    auto ids = clean_discord_user_ids("user:abc,USER:def");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "abc");
    EXPECT_EQ(ids[1], "def");
}

TEST(SetupHelpers_Discord, SkipsBlanks) {
    auto ids = clean_discord_user_ids("123,, ,456");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "123");
    EXPECT_EQ(ids[1], "456");
}

// ---------------------------------------------------------------------------
// Pretty-print formatters.
// ---------------------------------------------------------------------------

TEST(SetupHelpers_Format, HeaderContainsTitle) {
    EXPECT_NE(format_header("Test").find("Test"), std::string::npos);
}

TEST(SetupHelpers_Format, SuccessMarker) {
    auto s = format_success("ok");
    EXPECT_NE(s.find("ok"), std::string::npos);
}

TEST(SetupHelpers_Format, NonInteractiveGuidanceContainsSetupHints) {
    auto s = format_noninteractive_setup_guidance("stdin not a tty");
    EXPECT_NE(s.find("stdin not a tty"), std::string::npos);
    EXPECT_NE(s.find("hermes config set model.provider"), std::string::npos);
    EXPECT_NE(s.find("OPENROUTER_API_KEY"), std::string::npos);
}

TEST(SetupHelpers_Format, NonInteractiveGuidanceWorksWithoutReason) {
    auto s = format_noninteractive_setup_guidance("");
    EXPECT_NE(s.find("Non-interactive mode"), std::string::npos);
}
