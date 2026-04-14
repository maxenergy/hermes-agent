// Tests for send_message_tool — target parsing, ref parsing per
// platform, secret redaction, media extraction, and cron-skip detection.
#include "hermes/tools/registry.hpp"
#include "hermes/tools/send_message_tool.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace hermes::tools;

namespace {

class SendMessageToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_send_message_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("HERMES_CRON_AUTO_DELIVER_PLATFORM");
        unsetenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID");
        unsetenv("HERMES_CRON_AUTO_DELIVER_THREAD_ID");
    }
};

TEST(ParseTargetTest, HappyPath) {
    auto pt = parse_target("telegram:12345:67890");
    EXPECT_EQ(pt.platform, "telegram");
    EXPECT_EQ(pt.chat_id, "12345");
    EXPECT_EQ(pt.thread_id, "67890");
}

TEST(ParseTargetTest, EmptyThreadIdAllowed) {
    auto pt = parse_target("discord:chan123:");
    EXPECT_EQ(pt.platform, "discord");
    EXPECT_EQ(pt.chat_id, "chan123");
    EXPECT_EQ(pt.thread_id, "");
}

TEST(ParseTargetTest, InvalidFormatThrows) {
    EXPECT_THROW(parse_target("nocolons"), std::invalid_argument);
    EXPECT_THROW(parse_target("one:colon"), std::invalid_argument);
    EXPECT_THROW(parse_target(":empty_platform:tid"), std::invalid_argument);
}

// ── parse_target_ref ──────────────────────────────────────────────────

TEST(ParseTargetRef, TelegramNumeric) {
    auto r = parse_target_ref("telegram", "-1001234567890");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "-1001234567890");
    EXPECT_FALSE(r.thread_id.has_value());
}

TEST(ParseTargetRef, TelegramWithTopic) {
    auto r = parse_target_ref("telegram", "-1001234567890:17585");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "-1001234567890");
    EXPECT_EQ(*r.thread_id, "17585");
}

TEST(ParseTargetRef, FeishuOc) {
    auto r = parse_target_ref("feishu", "oc_abc-123");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "oc_abc-123");
}

TEST(ParseTargetRef, FeishuWithThread) {
    auto r = parse_target_ref("feishu", "oc_chat-id:ou_tid");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "oc_chat-id");
    EXPECT_EQ(*r.thread_id, "ou_tid");
}

TEST(ParseTargetRef, DiscordSnowflake) {
    auto r = parse_target_ref("discord", "999888777666555444");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "999888777666555444");
}

TEST(ParseTargetRef, WeixinWxid) {
    auto r = parse_target_ref("weixin", "wxid_abcdef_123");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "wxid_abcdef_123");
}

TEST(ParseTargetRef, WeixinChatroom) {
    auto r = parse_target_ref("weixin", "12345abc@chatroom");
    EXPECT_TRUE(r.is_explicit);
    EXPECT_EQ(*r.chat_id, "12345abc@chatroom");
}

TEST(ParseTargetRef, FriendlyNameIsNotExplicit) {
    auto r = parse_target_ref("slack", "#general");
    EXPECT_FALSE(r.is_explicit);
    EXPECT_FALSE(r.chat_id.has_value());
}

// ── sanitize_error_text ───────────────────────────────────────────────

TEST(SanitizeError, UrlQueryRedacted) {
    auto s = sanitize_error_text(
        "fetching https://api.example.com/x?access_token=SECRET123&foo=1");
    EXPECT_NE(s.find("access_token=***"), std::string::npos);
    EXPECT_EQ(s.find("SECRET123"), std::string::npos);
}

TEST(SanitizeError, GenericApiKeyRedacted) {
    auto s = sanitize_error_text("config api_key = sk-abc123def456");
    EXPECT_NE(s.find("api_key=***"), std::string::npos);
    EXPECT_EQ(s.find("sk-abc123def456"), std::string::npos);
}

TEST(SanitizeError, BearerTokenRedacted) {
    auto s = sanitize_error_text("Authorization: Bearer abcdef1234567890");
    EXPECT_NE(s.find("Bearer ***"), std::string::npos);
    EXPECT_EQ(s.find("abcdef1234567890"), std::string::npos);
}

// ── media extraction ──────────────────────────────────────────────────

TEST(MediaExtract, NoMediaReturnsOriginal) {
    auto [clean, media] = extract_media_directives("plain text");
    EXPECT_EQ(clean, "plain text");
    EXPECT_TRUE(media.empty());
}

TEST(MediaExtract, SingleImageDirective) {
    auto [clean, media] =
        extract_media_directives("hi [media:/tmp/cat.png]");
    EXPECT_EQ(clean, "hi");
    ASSERT_EQ(media.size(), 1u);
    EXPECT_EQ(media[0].path, "/tmp/cat.png");
    EXPECT_FALSE(media[0].is_voice);
}

TEST(MediaExtract, VoiceDirective) {
    auto [clean, media] = extract_media_directives("[voice:/tmp/v.opus]");
    EXPECT_EQ(clean, "");
    ASSERT_EQ(media.size(), 1u);
    EXPECT_TRUE(media[0].is_voice);
    EXPECT_EQ(describe_media_for_mirror(media), "[Sent voice message]");
}

TEST(MediaExtract, MultipleMedia) {
    auto [clean, media] = extract_media_directives(
        "hello [media:/a.jpg] world [media:/b.mp4]");
    ASSERT_EQ(media.size(), 2u);
    EXPECT_EQ(describe_media_for_mirror(media),
              "[Sent 2 media attachments]");
    EXPECT_EQ(clean, "hello  world");
}

TEST(MirrorText, ImageAttachment) {
    std::vector<MediaFile> m = {{"/tmp/x.png", false}};
    EXPECT_EQ(describe_media_for_mirror(m), "[Sent image attachment]");
}

TEST(MirrorText, VideoAttachment) {
    std::vector<MediaFile> m = {{"/tmp/x.MP4", false}};
    EXPECT_EQ(describe_media_for_mirror(m), "[Sent video attachment]");
}

TEST(MirrorText, DocumentAttachment) {
    std::vector<MediaFile> m = {{"/tmp/x.pdf", false}};
    EXPECT_EQ(describe_media_for_mirror(m), "[Sent document attachment]");
}

TEST(MirrorText, EmptyReturnsEmpty) {
    EXPECT_EQ(describe_media_for_mirror({}), "");
}

// ── cron auto-deliver skip ────────────────────────────────────────────

TEST(CronSkip, UnsetReturnsEmpty) {
    unsetenv("HERMES_CRON_AUTO_DELIVER_PLATFORM");
    unsetenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID");
    EXPECT_FALSE(get_cron_auto_delivery_target().has_value());
    EXPECT_TRUE(
        maybe_skip_cron_duplicate_send("telegram", "123", std::nullopt).empty());
}

TEST(CronSkip, MatchingTargetProducesSkip) {
    setenv("HERMES_CRON_AUTO_DELIVER_PLATFORM", "telegram", 1);
    setenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID", "999", 1);
    unsetenv("HERMES_CRON_AUTO_DELIVER_THREAD_ID");

    auto t = get_cron_auto_delivery_target();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->platform, "telegram");
    EXPECT_EQ(t->chat_id, "999");

    auto skip =
        maybe_skip_cron_duplicate_send("telegram", "999", std::nullopt);
    ASSERT_FALSE(skip.empty());
    auto j = nlohmann::json::parse(skip);
    EXPECT_TRUE(j["skipped"].get<bool>());
    EXPECT_EQ(j["target"].get<std::string>(), "telegram:999");

    unsetenv("HERMES_CRON_AUTO_DELIVER_PLATFORM");
    unsetenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID");
}

TEST(CronSkip, DifferentTargetNoSkip) {
    setenv("HERMES_CRON_AUTO_DELIVER_PLATFORM", "telegram", 1);
    setenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID", "999", 1);

    auto skip =
        maybe_skip_cron_duplicate_send("discord", "999", std::nullopt);
    EXPECT_TRUE(skip.empty());

    auto skip2 =
        maybe_skip_cron_duplicate_send("telegram", "888", std::nullopt);
    EXPECT_TRUE(skip2.empty());

    unsetenv("HERMES_CRON_AUTO_DELIVER_PLATFORM");
    unsetenv("HERMES_CRON_AUTO_DELIVER_CHAT_ID");
}

// ── Tool dispatch ─────────────────────────────────────────────────────

TEST_F(SendMessageToolTest, ListReturnsGatewayNotRunning) {
    auto result = ToolRegistry::instance().dispatch(
        "send_message", {{"action", "list"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("gateway not running"),
              std::string::npos);
}

TEST_F(SendMessageToolTest, SendWithoutRunnerReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "send_message",
        {{"action", "send"},
         {"target", "telegram:123:456"},
         {"message", "hello"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(SendMessageToolTest, UnknownPlatformErrors) {
    auto result = ToolRegistry::instance().dispatch(
        "send_message",
        {{"action", "send"},
         {"target", "pigeon:12345"},
         {"message", "hi"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("Unknown platform"),
              std::string::npos);
}

TEST_F(SendMessageToolTest, MissingTargetOrMessageErrors) {
    auto r1 = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "send_message",
            {{"action", "send"}, {"message", "hi"}}, {}));
    EXPECT_TRUE(r1.contains("error"));

    auto r2 = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "send_message",
            {{"action", "send"}, {"target", "telegram:1:2"}}, {}));
    EXPECT_TRUE(r2.contains("error"));
}

TEST_F(SendMessageToolTest, UnknownActionErrors) {
    auto r = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "send_message", {{"action", "dance"}}, {}));
    EXPECT_TRUE(r.contains("error"));
}

TEST(SupportedPlatforms, ContainsExpected) {
    const auto& p = supported_platforms();
    EXPECT_NE(std::find(p.begin(), p.end(), "telegram"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "discord"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "signal"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "matrix"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "feishu"), p.end());
}

}  // namespace
