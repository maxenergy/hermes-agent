// Tests for hermes/tools/send_message_tool_depth.hpp.
#include "hermes/tools/send_message_tool_depth.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace hermes::tools::send_message::depth;

TEST(SendMessageDepthAction, ParsesCanonicalValues) {
    EXPECT_EQ(parse_action("send"), Action::Send);
    EXPECT_EQ(parse_action("list"), Action::List);
    EXPECT_EQ(parse_action(""), Action::Send);
    EXPECT_EQ(parse_action("   "), Action::Send);
    EXPECT_EQ(parse_action("SEND"), Action::Send);
    EXPECT_EQ(parse_action("  List  "), Action::List);
    EXPECT_EQ(parse_action("delete"), Action::Unknown);
    EXPECT_EQ(parse_action("Send something"), Action::Unknown);
}

TEST(SendMessageDepthAction, NameRoundTrip) {
    EXPECT_EQ(action_name(Action::Send), "send");
    EXPECT_EQ(action_name(Action::List), "list");
    EXPECT_EQ(action_name(Action::Unknown), "unknown");
}

TEST(SendMessageDepthPlatform, KnownList) {
    const auto& names = known_platforms();
    EXPECT_EQ(names.size(), 8u);
    EXPECT_EQ(names.front(), "telegram");
    EXPECT_NE(std::find(names.begin(), names.end(), "signal"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "weixin"), names.end());
}

TEST(SendMessageDepthPlatform, Normalise) {
    EXPECT_EQ(normalise_platform_name("Telegram"), "telegram");
    EXPECT_EQ(normalise_platform_name("  DISCORD  "), "discord");
    EXPECT_EQ(normalise_platform_name("slack"), "slack");
    EXPECT_EQ(normalise_platform_name(""), "");
    EXPECT_EQ(normalise_platform_name("myspace"), "");
    EXPECT_EQ(normalise_platform_name("telegram "), "telegram");
}

TEST(SendMessageDepthPlatform, IsKnownExactMatch) {
    EXPECT_TRUE(is_known_platform("telegram"));
    EXPECT_TRUE(is_known_platform("feishu"));
    EXPECT_FALSE(is_known_platform("Telegram"));  // already-normalised only
    EXPECT_FALSE(is_known_platform(""));
}

TEST(SendMessageDepthSplit, NoColon) {
    auto s = split_target("telegram");
    EXPECT_EQ(s.platform, "telegram");
    EXPECT_EQ(s.remainder, "");
    EXPECT_FALSE(s.has_colon);
}

TEST(SendMessageDepthSplit, WithColon) {
    auto s = split_target("discord:#bot-home");
    EXPECT_EQ(s.platform, "discord");
    EXPECT_EQ(s.remainder, "#bot-home");
    EXPECT_TRUE(s.has_colon);
}

TEST(SendMessageDepthSplit, UnknownPlatform) {
    auto s = split_target("foo:bar:baz");
    EXPECT_EQ(s.platform, "");
    EXPECT_EQ(s.remainder, "bar:baz");
    EXPECT_TRUE(s.has_colon);
}

TEST(SendMessageDepthSplit, WhitespaceNormalised) {
    auto s = split_target("  Telegram :  -100 ");
    EXPECT_EQ(s.platform, "telegram");
    // Full-string is trimmed; internal whitespace around the remainder is
    // preserved so callers can decide how to strip.
    EXPECT_EQ(s.remainder, "  -100");
}

TEST(SendMessageDepthFormat, LabelWithoutThread) {
    EXPECT_EQ(format_target_label("telegram", "-1001234567890", std::nullopt),
              "telegram:-1001234567890");
}

TEST(SendMessageDepthFormat, LabelWithThread) {
    std::string_view tid{"17585"};
    EXPECT_EQ(format_target_label("telegram", "-1001234567890",
                                  std::optional<std::string_view>{tid}),
              "telegram:-1001234567890:17585");
}

TEST(SendMessageDepthFormat, EmptyThreadOmitted) {
    std::string_view tid{""};
    EXPECT_EQ(format_target_label("slack", "C01", std::optional<std::string_view>{tid}),
              "slack:C01");
}

TEST(SendMessageDepthMedia, ImageClassification) {
    EXPECT_EQ(classify_media("/tmp/a.jpg", false), MediaKind::Image);
    EXPECT_EQ(classify_media("/tmp/a.JPG", false), MediaKind::Image);
    EXPECT_EQ(classify_media("file.webp", false), MediaKind::Image);
    EXPECT_EQ(classify_media("file.PNG", false), MediaKind::Image);
}

TEST(SendMessageDepthMedia, VideoClassification) {
    EXPECT_EQ(classify_media("clip.mp4", false), MediaKind::Video);
    EXPECT_EQ(classify_media("story.MOV", false), MediaKind::Video);
}

TEST(SendMessageDepthMedia, AudioClassification) {
    EXPECT_EQ(classify_media("song.mp3", false), MediaKind::Audio);
    EXPECT_EQ(classify_media("sample.wav", false), MediaKind::Audio);
    EXPECT_EQ(classify_media("track.m4a", false), MediaKind::Audio);
}

TEST(SendMessageDepthMedia, VoiceTakesPrecedenceWhenFlagged) {
    EXPECT_EQ(classify_media("rec.ogg", true), MediaKind::Voice);
    EXPECT_EQ(classify_media("rec.opus", true), MediaKind::Voice);
    // Without the flag, .ogg is audio.
    EXPECT_EQ(classify_media("rec.ogg", false), MediaKind::Audio);
}

TEST(SendMessageDepthMedia, UnknownExtensionIsDocument) {
    EXPECT_EQ(classify_media("note.pdf", false), MediaKind::Document);
    EXPECT_EQ(classify_media("archive.zip", false), MediaKind::Document);
    EXPECT_EQ(classify_media("no-extension", false), MediaKind::Document);
    EXPECT_EQ(classify_media("/path/to.hidden/file", false),
              MediaKind::Document);
}

TEST(SendMessageDepthMedia, LabelStrings) {
    EXPECT_EQ(media_kind_label(MediaKind::Image), "[Sent image attachment]");
    EXPECT_EQ(media_kind_label(MediaKind::Video), "[Sent video attachment]");
    EXPECT_EQ(media_kind_label(MediaKind::Audio), "[Sent audio attachment]");
    EXPECT_EQ(media_kind_label(MediaKind::Voice), "[Sent voice message]");
    EXPECT_EQ(media_kind_label(MediaKind::Document),
              "[Sent document attachment]");
    EXPECT_EQ(media_kind_label(MediaKind::Unknown), "[Sent attachment]");
}

TEST(SendMessageDepthScrub, UrlQueryRedaction) {
    std::string in{"http://example.com/api?access_token=abc123&foo=bar"};
    std::string out{scrub_url_query_secrets(in)};
    EXPECT_NE(out.find("access_token=***"), std::string::npos);
    EXPECT_EQ(out.find("abc123"), std::string::npos);
    EXPECT_NE(out.find("foo=bar"), std::string::npos);
}

TEST(SendMessageDepthScrub, UrlQueryCaseInsensitive) {
    std::string out{scrub_url_query_secrets(
        "https://x/?ACCESS_TOKEN=xyz&API_KEY=abc")};
    EXPECT_EQ(out.find("xyz"), std::string::npos);
    EXPECT_EQ(out.find("abc"), std::string::npos);
}

TEST(SendMessageDepthScrub, GenericAssignment) {
    std::string out{scrub_generic_secret_assignments(
        "request failed with api_key=SECRET123 and more text")};
    EXPECT_NE(out.find("api_key=***"), std::string::npos);
    EXPECT_EQ(out.find("SECRET123"), std::string::npos);
}

TEST(SendMessageDepthScrub, PipelineAppliesBoth) {
    std::string in{
        "url?token=ABC and signature=XYZ plus api_key=DEF"};
    std::string out{scrub_secret_patterns(in)};
    EXPECT_EQ(out.find("ABC"), std::string::npos);
    EXPECT_EQ(out.find("XYZ"), std::string::npos);
    EXPECT_EQ(out.find("DEF"), std::string::npos);
}

TEST(SendMessageDepthScrub, NoSecretsLeavesUnchanged) {
    EXPECT_EQ(scrub_secret_patterns("plain text, no secrets"),
              "plain text, no secrets");
}

TEST(SendMessageDepthResponse, MissingTarget) {
    auto r = missing_target_response();
    EXPECT_TRUE(r.contains("error"));
    std::string msg = r["error"].get<std::string>();
    EXPECT_NE(msg.find("No target"), std::string::npos);
}

TEST(SendMessageDepthResponse, MissingMessage) {
    auto r = missing_message_response();
    EXPECT_EQ(r["error"].get<std::string>(), "No message text provided.");
}

TEST(SendMessageDepthResponse, UnknownPlatformListsAll) {
    auto r = unknown_platform_response("myspace");
    std::string msg = r["error"].get<std::string>();
    EXPECT_NE(msg.find("myspace"), std::string::npos);
    EXPECT_NE(msg.find("telegram"), std::string::npos);
    EXPECT_NE(msg.find("discord"), std::string::npos);
}

TEST(SendMessageDepthResponse, SuccessWithoutThread) {
    auto r = success_response("telegram", "-100", std::nullopt, "hi");
    EXPECT_TRUE(r["success"].get<bool>());
    EXPECT_EQ(r["platform"].get<std::string>(), "telegram");
    EXPECT_EQ(r["chat_id"].get<std::string>(), "-100");
    EXPECT_EQ(r["target"].get<std::string>(), "telegram:-100");
    EXPECT_FALSE(r.contains("thread_id"));
}

TEST(SendMessageDepthResponse, SuccessWithThread) {
    std::string_view tid{"42"};
    auto r = success_response("discord", "999", std::optional{tid}, "hi");
    EXPECT_EQ(r["thread_id"].get<std::string>(), "42");
    EXPECT_EQ(r["target"].get<std::string>(), "discord:999:42");
}

TEST(SendMessageDepthResponse, CronDuplicate) {
    auto r = cron_duplicate_skip_response("telegram", "-100", std::nullopt);
    EXPECT_TRUE(r["skipped"].get<bool>());
    EXPECT_EQ(r["reason"].get<std::string>(),
              "cron_auto_delivery_duplicate_target");
    EXPECT_EQ(r["target"].get<std::string>(), "telegram:-100");
}

TEST(SendMessageDepthChannelList, CapUnderLimit) {
    std::vector<std::string> in{"a", "b", "c"};
    auto out = cap_channel_list(in, 10);
    EXPECT_EQ(out, in);
}

TEST(SendMessageDepthChannelList, CapOverLimit) {
    std::vector<std::string> in{"a", "b", "c", "d", "e"};
    auto out = cap_channel_list(in, 2);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "a");
    EXPECT_EQ(out[1], "b");
    EXPECT_NE(out[2].find("3 more"), std::string::npos);
}

TEST(SendMessageDepthChannelList, CapZeroMeansNoCap) {
    std::vector<std::string> in{"a", "b"};
    EXPECT_EQ(cap_channel_list(in, 0), in);
}

TEST(SendMessageDepthChannelList, RenderJoins) {
    EXPECT_EQ(render_channel_list({"a", "b", "c"}), "a\nb\nc");
    EXPECT_EQ(render_channel_list({}), "");
    EXPECT_EQ(render_channel_list({"only"}), "only");
}

TEST(SendMessageDepthTelegram, NumericChatOnly) {
    auto t = parse_telegram_topic("-1001234567890");
    EXPECT_TRUE(t.matched);
    EXPECT_EQ(t.chat_id, "-1001234567890");
    EXPECT_FALSE(t.thread_id.has_value());
}

TEST(SendMessageDepthTelegram, ChatAndThread) {
    auto t = parse_telegram_topic("-100:42");
    EXPECT_TRUE(t.matched);
    EXPECT_EQ(t.chat_id, "-100");
    ASSERT_TRUE(t.thread_id.has_value());
    EXPECT_EQ(*t.thread_id, "42");
}

TEST(SendMessageDepthTelegram, RejectsNonNumeric) {
    EXPECT_FALSE(parse_telegram_topic("abc").matched);
    EXPECT_FALSE(parse_telegram_topic("oc_123").matched);
    EXPECT_FALSE(parse_telegram_topic("").matched);
}

TEST(SendMessageDepthTelegram, WhitespaceTrimmed) {
    auto t = parse_telegram_topic("  -100  ");
    EXPECT_TRUE(t.matched);
    EXPECT_EQ(t.chat_id, "-100");
}

TEST(SendMessageDepthDiscord, MatchesNumeric) {
    auto t = parse_discord_target("999888777:555444");
    EXPECT_TRUE(t.matched);
    EXPECT_EQ(t.chat_id, "999888777");
    ASSERT_TRUE(t.thread_id.has_value());
    EXPECT_EQ(*t.thread_id, "555444");
}

namespace {

std::optional<std::string> empty_env(std::string_view) { return std::nullopt; }

std::optional<std::string> stub_platform(std::string_view k) {
    if (k == "HERMES_CRON_AUTO_DELIVER_PLATFORM") return std::string{"Telegram"};
    if (k == "HERMES_CRON_AUTO_DELIVER_CHAT_ID") return std::string{" -100 "};
    return std::nullopt;
}

std::optional<std::string> stub_full(std::string_view k) {
    if (k == "HERMES_CRON_AUTO_DELIVER_PLATFORM") return std::string{"discord"};
    if (k == "HERMES_CRON_AUTO_DELIVER_CHAT_ID") return std::string{"999"};
    if (k == "HERMES_CRON_AUTO_DELIVER_THREAD_ID") return std::string{"42"};
    return std::nullopt;
}

std::optional<std::string> stub_empty_chat(std::string_view k) {
    if (k == "HERMES_CRON_AUTO_DELIVER_PLATFORM") return std::string{"telegram"};
    if (k == "HERMES_CRON_AUTO_DELIVER_CHAT_ID") return std::string{"   "};
    return std::nullopt;
}

}  // namespace

TEST(SendMessageDepthCron, NullLookup) {
    EXPECT_FALSE(cron_auto_target_from_env(nullptr).has_value());
}

TEST(SendMessageDepthCron, EmptyEnv) {
    EXPECT_FALSE(cron_auto_target_from_env(empty_env).has_value());
}

TEST(SendMessageDepthCron, PlatformAndChatOnly) {
    auto t = cron_auto_target_from_env(stub_platform);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->platform, "telegram");
    EXPECT_EQ(t->chat_id, "-100");
    EXPECT_FALSE(t->thread_id.has_value());
}

TEST(SendMessageDepthCron, FullTarget) {
    auto t = cron_auto_target_from_env(stub_full);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->platform, "discord");
    EXPECT_EQ(t->chat_id, "999");
    ASSERT_TRUE(t->thread_id.has_value());
    EXPECT_EQ(*t->thread_id, "42");
}

TEST(SendMessageDepthCron, EmptyChatIsNone) {
    EXPECT_FALSE(cron_auto_target_from_env(stub_empty_chat).has_value());
}
