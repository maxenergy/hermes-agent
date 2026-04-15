// Tests for the C++17 port of `hermes_cli/webhook.py`.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "hermes/cli/webhook_cmd.hpp"

using namespace hermes::cli::webhook_cmd;
using json = nlohmann::json;

TEST(WebhookCmd, NormalizeLowercasesAndSpaces) {
    EXPECT_EQ(normalize_subscription_name("  Deploy Notifier "),
              "deploy-notifier");
    EXPECT_EQ(normalize_subscription_name("GITHUB"), "github");
    EXPECT_EQ(normalize_subscription_name("Already-OK"), "already-ok");
}

TEST(WebhookCmd, ValidateAcceptsLowercaseAlnum) {
    EXPECT_TRUE(is_valid_subscription_name("a"));
    EXPECT_TRUE(is_valid_subscription_name("a_b-c"));
    EXPECT_TRUE(is_valid_subscription_name("deploy1"));
    EXPECT_TRUE(is_valid_subscription_name("7-segment"));
}

TEST(WebhookCmd, ValidateRejectsInvalid) {
    EXPECT_FALSE(is_valid_subscription_name(""));
    EXPECT_FALSE(is_valid_subscription_name("-leading"));
    EXPECT_FALSE(is_valid_subscription_name("_leading"));
    EXPECT_FALSE(is_valid_subscription_name("UPPER"));
    EXPECT_FALSE(is_valid_subscription_name("has space"));
    EXPECT_FALSE(is_valid_subscription_name("sym$bol"));
}

TEST(WebhookCmd, ParseCsvEmpty) {
    EXPECT_TRUE(parse_csv_field("").empty());
}

TEST(WebhookCmd, ParseCsvSingle) {
    auto out = parse_csv_field("push");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "push");
}

TEST(WebhookCmd, ParseCsvMultipleTrims) {
    auto out = parse_csv_field("push,  pull_request ,release");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "push");
    EXPECT_EQ(out[1], "pull_request");
    EXPECT_EQ(out[2], "release");
}

TEST(WebhookCmd, ParseCsvKeepsEmptyTokens) {
    // Python `[e.strip() for e in s.split(",")]` preserves empties.
    auto out = parse_csv_field("push,,release");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1], "");
}

TEST(WebhookCmd, BuildBaseUrlAliasesZeros) {
    EXPECT_EQ(build_webhook_base_url("0.0.0.0", 8644),
              "http://localhost:8644");
}

TEST(WebhookCmd, BuildBaseUrlPreservesCustomHost) {
    EXPECT_EQ(build_webhook_base_url("example.com", 443),
              "http://example.com:443");
}

TEST(WebhookCmd, BuildSubscriptionUrl) {
    EXPECT_EQ(build_subscription_url("http://localhost:8644", "github"),
              "http://localhost:8644/webhooks/github");
}

TEST(WebhookCmd, BuildRecordDefaults) {
    subscribe_inputs inputs{};
    inputs.name = "github";
    inputs.created_at_iso = "2026-04-13T00:00:00Z";
    inputs.secret = "abc";
    auto rec = build_subscription_record(inputs);
    EXPECT_EQ(rec["description"], "Agent-created subscription: github");
    EXPECT_EQ(rec["events"], json::array());
    EXPECT_EQ(rec["secret"], "abc");
    EXPECT_EQ(rec["deliver"], "log");
    EXPECT_EQ(rec["created_at"], "2026-04-13T00:00:00Z");
    EXPECT_EQ(rec.contains("deliver_extra"), false);
}

TEST(WebhookCmd, BuildRecordPopulatesEvents) {
    subscribe_inputs inputs{};
    inputs.name = "github";
    inputs.events_csv = "push, release";
    inputs.deliver = "telegram";
    inputs.deliver_chat_id = "123";
    inputs.description = "CI events";
    inputs.prompt = "Process";
    inputs.skills_csv = "s1, s2";
    inputs.created_at_iso = "2026-04-13T00:00:00Z";
    auto rec = build_subscription_record(inputs);
    EXPECT_EQ(rec["events"].size(), 2u);
    EXPECT_EQ(rec["skills"].size(), 2u);
    EXPECT_EQ(rec["deliver"], "telegram");
    EXPECT_EQ(rec["deliver_extra"]["chat_id"], "123");
    EXPECT_EQ(rec["description"], "CI events");
}

TEST(WebhookCmd, FormatSignatureHeader) {
    EXPECT_EQ(format_signature_header("deadbeef"), "sha256=deadbeef");
}

TEST(WebhookCmd, TruncatePromptPreviewShort) {
    EXPECT_EQ(truncate_prompt_preview("short"), "short");
}

TEST(WebhookCmd, TruncatePromptPreviewLong) {
    std::string prompt(120, 'a');
    auto preview = truncate_prompt_preview(prompt);
    EXPECT_EQ(preview.size(), 83u);
    EXPECT_EQ(preview.substr(preview.size() - 3), "...");
}

TEST(WebhookCmd, TruncatePromptPreviewBoundary) {
    std::string prompt(80, 'a');
    EXPECT_EQ(truncate_prompt_preview(prompt), prompt);
    prompt.push_back('b');
    auto out = truncate_prompt_preview(prompt);
    EXPECT_EQ(out.size(), 83u);
}

TEST(WebhookCmd, FormatListEntryBasic) {
    json route{{"description", "desc"},
               {"events", json::array({"push"})},
               {"deliver", "log"}};
    auto lines = format_list_entry("gh", route, "http://x");
    EXPECT_EQ(lines[0], "  * gh");
    EXPECT_EQ(lines[1], "    desc");
    EXPECT_EQ(lines[2], "    URL:     http://x/webhooks/gh");
    EXPECT_EQ(lines[3], "    Events:  push");
    EXPECT_EQ(lines[4], "    Deliver: log");
}

TEST(WebhookCmd, FormatListEntryEmptyEventsShowsAll) {
    json route{{"description", ""},
               {"events", json::array()},
               {"deliver", "log"}};
    auto lines = format_list_entry("gh", route, "http://x");
    bool saw{false};
    for (const auto& l : lines) {
        if (l == "    Events:  (all)") {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(WebhookCmd, FormatSubscribeSummaryCreated) {
    json route{{"secret", "topsecret"},
               {"events", json::array({"push"})},
               {"deliver", "telegram"},
               {"prompt", "short prompt"}};
    auto lines = format_subscribe_summary("gh", route, "http://h", false);
    bool saw_created{false};
    bool saw_events{false};
    for (const auto& l : lines) {
        if (l == "  Created webhook subscription: gh") saw_created = true;
        if (l == "  Events: push") saw_events = true;
    }
    EXPECT_TRUE(saw_created);
    EXPECT_TRUE(saw_events);
}

TEST(WebhookCmd, FormatSubscribeSummaryUpdated) {
    json route{{"secret", ""}, {"events", json::array()}, {"deliver", "log"}};
    auto lines = format_subscribe_summary("gh", route, "http://h", true);
    bool saw_updated{false};
    bool saw_all{false};
    for (const auto& l : lines) {
        if (l == "  Updated webhook subscription: gh") saw_updated = true;
        if (l == "  Events: (all)") saw_all = true;
    }
    EXPECT_TRUE(saw_updated);
    EXPECT_TRUE(saw_all);
}

TEST(WebhookCmd, SetupHintContainsHermesHome) {
    auto hint = webhook_setup_hint("~/.hermes/profiles/work");
    EXPECT_NE(hint.find("~/.hermes/profiles/work/config.yaml"),
              std::string::npos);
    EXPECT_NE(hint.find("hermes gateway run"), std::string::npos);
    EXPECT_NE(hint.find("WEBHOOK_ENABLED=true"), std::string::npos);
}
