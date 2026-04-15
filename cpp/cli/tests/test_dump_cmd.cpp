// Tests for the C++17 port of `hermes_cli/dump.py`.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "hermes/cli/dump_cmd.hpp"

using namespace hermes::cli::dump_cmd;
using json = nlohmann::json;

TEST(DumpCmd, RedactEmpty) {
    EXPECT_EQ(redact_secret(""), "");
}

TEST(DumpCmd, RedactShortBecomesStars) {
    EXPECT_EQ(redact_secret("short"), "***");
    EXPECT_EQ(redact_secret("12345678901"), "***");  // 11 chars
}

TEST(DumpCmd, RedactExactBoundary) {
    // 12 chars is the threshold -- should preview.
    EXPECT_EQ(redact_secret("abcd12345678"), "abcd...5678");
}

TEST(DumpCmd, RedactLong) {
    EXPECT_EQ(redact_secret("sk-abcdefghij1234567890"),
              "sk-a...7890");
}

TEST(DumpCmd, CountMcpEmpty) {
    EXPECT_EQ(count_mcp_servers(json::object()), 0u);
}

TEST(DumpCmd, CountMcpWithServers) {
    json cfg{{"mcp", {{"servers", {{"a", json::object()},
                                    {"b", json::object()}}}}}};
    EXPECT_EQ(count_mcp_servers(cfg), 2u);
}

TEST(DumpCmd, CountMcpMalformed) {
    EXPECT_EQ(count_mcp_servers(json{{"mcp", "not-a-dict"}}), 0u);
    EXPECT_EQ(count_mcp_servers(json{{"mcp", {{"servers", "nope"}}}}), 0u);
    EXPECT_EQ(count_mcp_servers(json::array()), 0u);
}

TEST(DumpCmd, CronSummaryMissingFile) {
    EXPECT_EQ(cron_summary(false, ""), "0");
}

TEST(DumpCmd, CronSummaryBadJson) {
    EXPECT_EQ(cron_summary(true, "{ not json"), "(error reading)");
}

TEST(DumpCmd, CronSummaryNoJobs) {
    EXPECT_EQ(cron_summary(true, "{}"), "0 active / 0 total");
}

TEST(DumpCmd, CronSummaryAllActive) {
    std::string body{R"({"jobs":[{"enabled":true},{"enabled":true}]})"};
    EXPECT_EQ(cron_summary(true, body), "2 active / 2 total");
}

TEST(DumpCmd, CronSummaryMixed) {
    std::string body{R"({"jobs":[{"enabled":true},{"enabled":false},{}]})"};
    // `{}` defaults enabled=true per the Python branch (j.get("enabled", True))
    EXPECT_EQ(cron_summary(true, body), "2 active / 3 total");
}

TEST(DumpCmd, CronSummaryNonArrayJobs) {
    EXPECT_EQ(cron_summary(true, R"({"jobs":"nope"})"), "(error reading)");
}

TEST(DumpCmd, ExtractModelDictWithDefault) {
    auto res = extract_model_and_provider(json{
        {"model", {{"default", "claude"}, {"provider", "nous"}}}});
    EXPECT_EQ(res.model, "claude");
    EXPECT_EQ(res.provider, "nous");
}

TEST(DumpCmd, ExtractModelDictFallsBackToName) {
    auto res = extract_model_and_provider(json{
        {"model", {{"name", "gpt-5"}}}});
    EXPECT_EQ(res.model, "gpt-5");
    EXPECT_EQ(res.provider, "(auto)");
}

TEST(DumpCmd, ExtractModelDictMissingAll) {
    auto res = extract_model_and_provider(json{{"model", json::object()}});
    EXPECT_EQ(res.model, "(not set)");
    EXPECT_EQ(res.provider, "(auto)");
}

TEST(DumpCmd, ExtractModelString) {
    auto res = extract_model_and_provider(json{{"model", "claude-haiku"}});
    EXPECT_EQ(res.model, "claude-haiku");
    EXPECT_EQ(res.provider, "(auto)");
}

TEST(DumpCmd, ExtractModelEmptyString) {
    auto res = extract_model_and_provider(json{{"model", ""}});
    EXPECT_EQ(res.model, "(not set)");
    EXPECT_EQ(res.provider, "(auto)");
}

TEST(DumpCmd, ExtractModelMissing) {
    auto res = extract_model_and_provider(json::object());
    EXPECT_EQ(res.model, "(not set)");
    EXPECT_EQ(res.provider, "(auto)");
}

TEST(DumpCmd, DetectMemoryProviderEmpty) {
    EXPECT_EQ(detect_memory_provider(json::object()), "built-in");
}

TEST(DumpCmd, DetectMemoryProviderSet) {
    EXPECT_EQ(detect_memory_provider(
                  json{{"memory", {{"provider", "mem0"}}}}),
              "mem0");
}

TEST(DumpCmd, DetectMemoryProviderEmptyProvider) {
    EXPECT_EQ(detect_memory_provider(
                  json{{"memory", {{"provider", ""}}}}),
              "built-in");
}

TEST(DumpCmd, PlatformEnvTableSize) {
    const auto& table = platform_env_table();
    EXPECT_EQ(table.size(), 14u);
    EXPECT_EQ(table[0].first, "telegram");
    EXPECT_EQ(table[0].second, "TELEGRAM_BOT_TOKEN");
    EXPECT_EQ(table.back().first, "weixin");
}

TEST(DumpCmd, DetectConfiguredPlatforms) {
    auto lookup = [](const std::string& name) -> std::string {
        if (name == "TELEGRAM_BOT_TOKEN") {
            return "123";
        }
        if (name == "SLACK_BOT_TOKEN") {
            return "xoxb";
        }
        return std::string{};
    };
    auto platforms = detect_configured_platforms(lookup);
    ASSERT_EQ(platforms.size(), 2u);
    EXPECT_EQ(platforms[0], "telegram");
    EXPECT_EQ(platforms[1], "slack");
}

TEST(DumpCmd, DetectConfiguredPlatformsAllMissing) {
    auto lookup = [](const std::string&) { return std::string{}; };
    EXPECT_TRUE(detect_configured_platforms(lookup).empty());
}

TEST(DumpCmd, ApiKeysTableCoversKnownKeys) {
    const auto& table = api_keys_table();
    EXPECT_EQ(table.size(), 22u);
    EXPECT_EQ(table[0].first, "OPENROUTER_API_KEY");
    EXPECT_EQ(table[0].second, "openrouter");
}

TEST(DumpCmd, RenderApiKeyLineNotSet) {
    std::string line{render_dump_api_key_line("openai", "", false)};
    EXPECT_EQ(line, "  openai               not set");
}

TEST(DumpCmd, RenderApiKeyLineSet) {
    std::string line{render_dump_api_key_line("openai", "sk-abc", false)};
    EXPECT_EQ(line, "  openai               set");
}

TEST(DumpCmd, RenderApiKeyLineShowKeysPreview) {
    std::string line{render_dump_api_key_line(
        "openai", "sk-abcdefghij1234567890", true)};
    EXPECT_EQ(line, "  openai               sk-a...7890");
}

TEST(DumpCmd, RenderApiKeyLineShowKeysEmptyFallsBack) {
    std::string line{render_dump_api_key_line("openai", "", true)};
    EXPECT_EQ(line, "  openai               not set");
}

TEST(DumpCmd, FormatVersionLineWithDateAndCommit) {
    EXPECT_EQ(format_version_line("0.1.0", "2026-04-13", "deadbeef"),
              "0.1.0 (2026-04-13) [deadbeef]");
}

TEST(DumpCmd, FormatVersionLineNoDate) {
    EXPECT_EQ(format_version_line("0.1.0", "", "deadbeef"),
              "0.1.0 [deadbeef]");
}

TEST(DumpCmd, InterestingPathsIncludesDisplaySkin) {
    const auto& paths = interesting_override_paths();
    bool found{false};
    for (const auto& [section, key] : paths) {
        if (section == "display" && key == "skin") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(DumpCmd, CollectOverridesEmpty) {
    auto out = collect_config_overrides(json::object(), json::object());
    EXPECT_TRUE(out.empty());
}

TEST(DumpCmd, CollectOverridesDetectsUserChange) {
    json defaults{{"agent", {{"max_turns", 50}}}};
    json cfg{{"agent", {{"max_turns", 200}}}};
    auto out = collect_config_overrides(cfg, defaults);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "agent.max_turns");
    EXPECT_EQ(out[0].second, "200");
}

TEST(DumpCmd, CollectOverridesSkipsMatchingValues) {
    json defaults{{"agent", {{"max_turns", 50}}}};
    json cfg{{"agent", {{"max_turns", 50}}}};
    auto out = collect_config_overrides(cfg, defaults);
    EXPECT_TRUE(out.empty());
}

TEST(DumpCmd, CollectOverridesBoolPythonStyle) {
    json defaults{{"compression", {{"enabled", true}}}};
    json cfg{{"compression", {{"enabled", false}}}};
    auto out = collect_config_overrides(cfg, defaults);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "compression.enabled");
    EXPECT_EQ(out[0].second, "False");
}

TEST(DumpCmd, CollectOverridesToolsets) {
    json defaults{{"toolsets", json::array({"hermes-cli"})}};
    json cfg{{"toolsets", json::array({"hermes-cli", "coder"})}};
    auto out = collect_config_overrides(cfg, defaults);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "toolsets");
    EXPECT_EQ(out[0].second, "['hermes-cli', 'coder']");
}

TEST(DumpCmd, CollectOverridesFallbacksEmptyIgnored) {
    json defaults{};
    json cfg{{"fallback_providers", json::array()}};
    auto out = collect_config_overrides(cfg, defaults);
    EXPECT_TRUE(out.empty());
}

TEST(DumpCmd, CollectOverridesFallbacksListed) {
    json defaults{};
    json cfg{{"fallback_providers", json::array({"openrouter", "anthropic"})}};
    auto out = collect_config_overrides(cfg, defaults);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "fallback_providers");
    EXPECT_EQ(out[0].second, "['openrouter', 'anthropic']");
}

TEST(DumpCmd, CollectOverridesSkipsNullUserValues) {
    json defaults{{"agent", {{"max_turns", 50}}}};
    json cfg{{"agent", {{"max_turns", nullptr}}}};
    auto out = collect_config_overrides(cfg, defaults);
    EXPECT_TRUE(out.empty());
}
