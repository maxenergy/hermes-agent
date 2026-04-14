// Unit tests for the full-depth TelegramAdapter port.
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

#include "../platforms/telegram.hpp"

using hermes::gateway::platforms::InlineKeyboardButton;
using hermes::gateway::platforms::InlineKeyboardMarkup;
using hermes::gateway::platforms::MediaGroupBuffer;
using hermes::gateway::platforms::ReplyKeyboardButton;
using hermes::gateway::platforms::ReplyKeyboardMarkup;
using hermes::gateway::platforms::TelegramAdapter;
using hermes::gateway::platforms::TelegramChatType;
using hermes::gateway::platforms::TelegramError;
using hermes::gateway::platforms::TelegramErrorKind;
using hermes::gateway::platforms::TelegramSendOptions;
using hermes::gateway::platforms::classify_telegram_error;
using hermes::gateway::platforms::format_message_markdown_v2;
using hermes::gateway::platforms::parse_chat_type;
using hermes::gateway::platforms::split_message_for_telegram;
using hermes::gateway::platforms::strip_markdown_v2;
using hermes::llm::FakeHttpTransport;

namespace {

std::string ok_body(const nlohmann::json& result) {
    return nlohmann::json{{"ok", true}, {"result", result}}.dump();
}

hermes::llm::HttpTransport::Response ok_response(const nlohmann::json& result) {
    return {200, ok_body(result), {}};
}

TelegramAdapter make_adapter(FakeHttpTransport* t, std::string token = "TKN") {
    TelegramAdapter::Config cfg;
    cfg.bot_token = std::move(token);
    cfg.max_send_retries = 1;  // keep tests fast
    return TelegramAdapter(cfg, t);
}

}  // namespace

// ── Chat type parsing ──────────────────────────────────────────────────────

TEST(TelegramFullParse, ChatTypeMapping) {
    EXPECT_EQ(parse_chat_type("private"), TelegramChatType::Private);
    EXPECT_EQ(parse_chat_type("GROUP"), TelegramChatType::Group);
    EXPECT_EQ(parse_chat_type("supergroup"), TelegramChatType::Supergroup);
    EXPECT_EQ(parse_chat_type("channel"), TelegramChatType::Channel);
    EXPECT_EQ(parse_chat_type("unknown_thing"), TelegramChatType::Unknown);
}

TEST(TelegramFullParse, ForumTopicAndMediaGroup) {
    nlohmann::json msg = {{"message_thread_id", 42},
                          {"media_group_id", "grp"}};
    EXPECT_EQ(TelegramAdapter::parse_forum_topic(msg).value(), 42);
    EXPECT_EQ(*TelegramAdapter::parse_media_group_id(msg), "grp");
    EXPECT_FALSE(TelegramAdapter::parse_forum_topic(nlohmann::json::object())
                     .has_value());
    EXPECT_FALSE(TelegramAdapter::parse_media_group_id(nlohmann::json::object())
                     .has_value());
}

TEST(TelegramFullParse, MessageChatType) {
    nlohmann::json msg = {{"chat", {{"type", "supergroup"}}}};
    EXPECT_EQ(TelegramAdapter::parse_message_chat_type(msg),
              TelegramChatType::Supergroup);
    EXPECT_EQ(TelegramAdapter::parse_message_chat_type(
                  nlohmann::json::object()),
              TelegramChatType::Unknown);
}

TEST(TelegramFullParse, UpdateKind) {
    nlohmann::json u = {{"update_id", 1}, {"callback_query", {{"id", "x"}}}};
    EXPECT_EQ(TelegramAdapter::parse_update_kind(u), "callback_query");
    nlohmann::json u2 = {{"update_id", 2}, {"message", {{"text", "hi"}}}};
    EXPECT_EQ(TelegramAdapter::parse_update_kind(u2), "message");
    EXPECT_EQ(TelegramAdapter::parse_update_kind(nlohmann::json::object()), "");
}

TEST(TelegramFullParse, ForumTopicServiceMessages) {
    EXPECT_TRUE(TelegramAdapter::is_forum_topic_created(
        nlohmann::json{{"forum_topic_created", {{"name", "x"}}}}));
    EXPECT_FALSE(TelegramAdapter::is_forum_topic_created(nlohmann::json::object()));
    EXPECT_TRUE(TelegramAdapter::is_forum_topic_closed(
        nlohmann::json{{"forum_topic_closed", nlohmann::json::object()}}));
}

TEST(TelegramFullParse, ReplyAndMigrateId) {
    nlohmann::json m = {
        {"reply_to_message", {{"message_id", 5}}},
        {"migrate_to_chat_id", -1001},
    };
    EXPECT_EQ(TelegramAdapter::parse_reply_to_message_id(m).value(), 5);
    EXPECT_EQ(TelegramAdapter::parse_migrate_to_chat_id(m).value(), -1001);
}

// ── MarkdownV2 formatting ──────────────────────────────────────────────────

TEST(TelegramFullMarkdown, EscapeAllSpecials) {
    auto out = TelegramAdapter::format_markdown_v2("foo.bar!");
    EXPECT_EQ(out, "foo\\.bar\\!");
}

TEST(TelegramFullMarkdown, StripRoundTrip) {
    auto esc = TelegramAdapter::format_markdown_v2("a.b_c");
    EXPECT_NE(esc, "a.b_c");
    EXPECT_EQ(strip_markdown_v2(esc), "a.b_c");
}

TEST(TelegramFullMarkdown, FencedCodeIsProtected) {
    std::string input = "start ```py\nx = 1\n``` end";
    auto out = format_message_markdown_v2(input);
    EXPECT_NE(out.find("```py"), std::string::npos);
    EXPECT_NE(out.find("x = 1"), std::string::npos);
}

TEST(TelegramFullMarkdown, HeadersBecomeBold) {
    auto out = format_message_markdown_v2("## Title here");
    EXPECT_NE(out.find("*Title here*"), std::string::npos);
}

TEST(TelegramFullMarkdown, BoldItalic) {
    auto out = format_message_markdown_v2("**bold** then *italic*");
    EXPECT_NE(out.find("*bold*"), std::string::npos);
    EXPECT_NE(out.find("_italic_"), std::string::npos);
}

TEST(TelegramFullMarkdown, EmptyStringOk) {
    EXPECT_EQ(format_message_markdown_v2(""), "");
}

// ── Message splitting ──────────────────────────────────────────────────────

TEST(TelegramFullSplit, ShortPassesThrough) {
    auto parts = split_message_for_telegram("hello world", 4096);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], "hello world");
}

TEST(TelegramFullSplit, AppendsSuffix) {
    // Force multiple parts with small max_len.
    std::string s(5000, 'x');
    auto parts = split_message_for_telegram(s, 2000);
    EXPECT_GT(parts.size(), 1u);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto expected_suffix = " (" + std::to_string(i + 1) + "/" +
                               std::to_string(parts.size()) + ")";
        EXPECT_NE(parts[i].find(expected_suffix), std::string::npos);
    }
}

TEST(TelegramFullSplit, CodeFenceAware) {
    std::string prefix(1500, 'a');
    std::string code(1500, 'b');
    std::string input = prefix + "\n```py\n" + code + "\n```\ntail";
    auto parts = split_message_for_telegram(input, 2000);
    ASSERT_GE(parts.size(), 2u);
    // Each chunk that contains an opened fence should be balanced by a close.
    for (const auto& p : parts) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = p.find("```", pos)) != std::string::npos) {
            ++count;
            pos += 3;
        }
        EXPECT_EQ(count % 2, 0u) << p;
    }
}

TEST(TelegramFullSplit, EmptyReturnsEmpty) {
    auto parts = split_message_for_telegram("", 4096);
    EXPECT_TRUE(parts.empty());
}

// ── Error classification ───────────────────────────────────────────────────

TEST(TelegramFullErrors, FloodWaitFromParameters) {
    std::string body = R"({"ok":false,"error_code":429,"description":"Too Many Requests: retry after 3","parameters":{"retry_after":3}})";
    auto err = classify_telegram_error(429, body);
    EXPECT_EQ(err.kind, TelegramErrorKind::FloodWait);
    EXPECT_DOUBLE_EQ(err.retry_after_seconds, 3.0);
}

TEST(TelegramFullErrors, FloodWaitFromDescription) {
    std::string body = R"({"ok":false,"error_code":429,"description":"Too Many Requests: retry after 5"})";
    auto err = classify_telegram_error(429, body);
    EXPECT_EQ(err.kind, TelegramErrorKind::FloodWait);
    EXPECT_DOUBLE_EQ(err.retry_after_seconds, 5.0);
}

TEST(TelegramFullErrors, ChatMigratedDetected) {
    std::string body = R"({"ok":false,"error_code":400,"description":"Bad Request: group chat was upgraded to a supergroup chat","parameters":{"migrate_to_chat_id":-1001}})";
    auto err = classify_telegram_error(400, body);
    EXPECT_EQ(err.kind, TelegramErrorKind::ChatMigrated);
    EXPECT_EQ(err.migrate_to_chat_id.value(), -1001);
}

TEST(TelegramFullErrors, MessageNotModifiedAndTooLong) {
    auto a = classify_telegram_error(
        400, R"({"ok":false,"description":"Bad Request: message is not modified"})");
    EXPECT_EQ(a.kind, TelegramErrorKind::NotModified);
    auto b = classify_telegram_error(
        400, R"({"ok":false,"description":"Bad Request: message is too long"})");
    EXPECT_EQ(b.kind, TelegramErrorKind::MessageTooLong);
}

TEST(TelegramFullErrors, ThreadAndReplyNotFound) {
    auto a = classify_telegram_error(
        400, R"({"description":"Bad Request: message thread not found"})");
    EXPECT_EQ(a.kind, TelegramErrorKind::ThreadNotFound);
    auto b = classify_telegram_error(
        400, R"({"description":"Bad Request: message to be replied not found"})");
    EXPECT_EQ(b.kind, TelegramErrorKind::ReplyNotFound);
}

TEST(TelegramFullErrors, UnauthorizedAndForbidden) {
    EXPECT_EQ(classify_telegram_error(401, "{}").kind,
              TelegramErrorKind::Unauthorized);
    EXPECT_EQ(classify_telegram_error(403, "{}").kind,
              TelegramErrorKind::Forbidden);
}

TEST(TelegramFullErrors, TransientOn5xxAnd0) {
    EXPECT_EQ(classify_telegram_error(500, "{}").kind,
              TelegramErrorKind::Transient);
    EXPECT_EQ(classify_telegram_error(0, "{}").kind,
              TelegramErrorKind::Transient);
}

// ── Media group correlation ────────────────────────────────────────────────

TEST(TelegramFullMediaGroup, AppendAndDrainById) {
    MediaGroupBuffer buf;
    EXPECT_TRUE(buf.append({{"media_group_id", "A"}, {"message_id", 1}}));
    EXPECT_FALSE(buf.append({{"media_group_id", "A"}, {"message_id", 2}}));
    EXPECT_FALSE(buf.append({{"media_group_id", "A"}, {"message_id", 3}}));
    auto drained = buf.drain("A");
    ASSERT_TRUE(drained.has_value());
    EXPECT_EQ(drained->size(), 3u);
    EXPECT_FALSE(buf.drain("A").has_value());
}

TEST(TelegramFullMediaGroup, DrainExpired) {
    MediaGroupBuffer buf;
    buf.append({{"media_group_id", "A"}, {"message_id", 1}});
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto groups = buf.drain_expired(std::chrono::milliseconds(10));
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].size(), 1u);
    EXPECT_EQ(buf.pending(), 0u);
}

TEST(TelegramFullMediaGroup, SingletonMessageCountedFirst) {
    MediaGroupBuffer buf;
    // No media_group_id — treated as fresh singleton.
    EXPECT_TRUE(buf.append({{"message_id", 1}}));
    EXPECT_EQ(buf.pending(), 0u);
}

// ── Callback data routing ──────────────────────────────────────────────────

TEST(TelegramFullCallback, ClassifyCallbackData) {
    EXPECT_EQ(TelegramAdapter::classify_callback_data("ea:once:42"), "ea");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("mp:openai"), "mp");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("mm:3"), "mm");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("mg:2"), "mg");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("mb"), "mb");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("mx"), "mx");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("update_prompt:y"),
              "update_prompt");
    EXPECT_EQ(TelegramAdapter::classify_callback_data("unknown"), "");
}

// ── Mention detection / gating ─────────────────────────────────────────────

TEST(TelegramFullMention, DetectsPlainAtMention) {
    nlohmann::json msg = {{"text", "hello @Hermesbot please"}};
    EXPECT_TRUE(TelegramAdapter::message_mentions_bot(msg, "hermesbot"));
    EXPECT_FALSE(
        TelegramAdapter::message_mentions_bot(msg, "otherbot"));
}

TEST(TelegramFullMention, DetectsTextMentionEntity) {
    nlohmann::json msg = {
        {"text", "hi"},
        {"entities",
         {{{"type", "text_mention"}, {"user", {{"id", 777}}}, {"offset", 0},
           {"length", 2}}}}};
    EXPECT_TRUE(TelegramAdapter::message_mentions_bot(msg, "", 777));
    EXPECT_FALSE(TelegramAdapter::message_mentions_bot(msg, "", 999));
}

TEST(TelegramFullMention, GroupRequireMentionGating) {
    FakeHttpTransport fake;
    TelegramAdapter::Config cfg;
    cfg.bot_token = "TKN";
    cfg.require_mention = true;
    TelegramAdapter adapter(cfg, &fake);
    adapter.set_bot_username("hermesbot");
    adapter.set_bot_id(777);

    nlohmann::json plain = {{"chat", {{"type", "supergroup"}, {"id", 1}}},
                            {"text", "random text"}};
    EXPECT_FALSE(adapter.should_process_message(plain));

    nlohmann::json mentioning = {{"chat", {{"type", "supergroup"}, {"id", 1}}},
                                  {"text", "hey @hermesbot help"}};
    EXPECT_TRUE(adapter.should_process_message(mentioning));

    nlohmann::json dm = {{"chat", {{"type", "private"}, {"id", 2}}},
                         {"text", "hello"}};
    EXPECT_TRUE(adapter.should_process_message(dm));
}

TEST(TelegramFullMention, CleanBotTriggerText) {
    EXPECT_EQ(TelegramAdapter::clean_bot_trigger_text("@HermesBot hello",
                                                     "HermesBot"),
              "hello");
    EXPECT_EQ(TelegramAdapter::clean_bot_trigger_text("no mention here",
                                                     "HermesBot"),
              "no mention here");
}

// ── Keyboard serialization ─────────────────────────────────────────────────

TEST(TelegramFullKeyboard, InlineKeyboardJsonShape) {
    InlineKeyboardMarkup kb;
    kb.rows.push_back({InlineKeyboardButton::data("Yes", "update_prompt:y"),
                       InlineKeyboardButton::link("Docs", "https://example")});
    auto j = kb.to_json();
    ASSERT_TRUE(j.contains("inline_keyboard"));
    ASSERT_TRUE(j["inline_keyboard"].is_array());
    EXPECT_EQ(j["inline_keyboard"][0][0]["callback_data"], "update_prompt:y");
    EXPECT_EQ(j["inline_keyboard"][0][1]["url"], "https://example");
}

TEST(TelegramFullKeyboard, ReplyKeyboardJsonShape) {
    ReplyKeyboardMarkup kb;
    ReplyKeyboardButton b;
    b.text = "Send location";
    b.request_location = true;
    kb.rows.push_back({b});
    kb.one_time_keyboard = true;
    auto j = kb.to_json();
    EXPECT_TRUE(j.value("one_time_keyboard", false));
    EXPECT_TRUE(j["keyboard"][0][0].value("request_location", false));
}

// ── End-to-end API calls via FakeHttpTransport ────────────────────────────

TEST(TelegramFullApi, SendMessagePostsCorrectPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 77}}));
    auto adapter = make_adapter(&fake);

    TelegramSendOptions opts;
    opts.parse_mode = "";  // no formatting
    auto res = adapter.send_message("chat-1", "hello world", opts);
    EXPECT_TRUE(res.ok);
    ASSERT_EQ(res.message_ids.size(), 1u);
    EXPECT_EQ(res.message_ids[0], 77);

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/sendMessage"), std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["chat_id"], "chat-1");
    EXPECT_EQ(body["text"], "hello world");
}

TEST(TelegramFullApi, SendMessageWithInlineKeyboard) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 1}}));
    auto adapter = make_adapter(&fake);

    TelegramSendOptions opts;
    opts.parse_mode = "";
    InlineKeyboardMarkup kb;
    kb.rows.push_back({InlineKeyboardButton::data("OK", "ok"),
                       InlineKeyboardButton::data("NO", "no")});
    opts.inline_keyboard = kb;

    EXPECT_TRUE(adapter.send_message("c1", "choose", opts).ok);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("reply_markup"));
    EXPECT_TRUE(body["reply_markup"].contains("inline_keyboard"));
}

TEST(TelegramFullApi, SendMessageEmptyContentSucceedsWithoutRequest) {
    FakeHttpTransport fake;
    auto adapter = make_adapter(&fake);
    TelegramSendOptions opts;
    opts.parse_mode = "";
    auto res = adapter.send_message("chat", "   ", opts);
    EXPECT_TRUE(res.ok);
    EXPECT_TRUE(fake.requests().empty());
}

TEST(TelegramFullApi, SendMessageNoTokenFails) {
    FakeHttpTransport fake;
    auto adapter = make_adapter(&fake, "");
    TelegramSendOptions opts;
    opts.parse_mode = "";
    auto res = adapter.send_message("chat", "hello", opts);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.last_error.kind, TelegramErrorKind::Unauthorized);
}

TEST(TelegramFullApi, EditMessageTextNotModifiedIsSuccess) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {400, R"({"ok":false,"description":"Bad Request: message is not modified"})", {}});
    auto adapter = make_adapter(&fake);
    auto res = adapter.edit_message_text("c1", 9, "same", "");
    EXPECT_TRUE(res.ok);
}

TEST(TelegramFullApi, DeleteMessageCallsEndpoint) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response(true));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(adapter.delete_message("c1", 9));
    EXPECT_NE(fake.requests()[0].url.find("/deleteMessage"),
              std::string::npos);
}

TEST(TelegramFullApi, ForwardAndCopy) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 10}}));
    fake.enqueue_response(ok_response({{"message_id", 11}}));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(adapter.forward_message("src", "dst", 5).ok);
    EXPECT_TRUE(adapter.copy_message("src", "dst", 5, std::string("cap")).ok);
    EXPECT_NE(fake.requests()[0].url.find("/forwardMessage"),
              std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("/copyMessage"), std::string::npos);
}

TEST(TelegramFullApi, PinUnpinAndReactions) {
    FakeHttpTransport fake;
    for (int i = 0; i < 4; ++i) fake.enqueue_response(ok_response(true));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(adapter.pin_chat_message("c", 1));
    EXPECT_TRUE(adapter.unpin_chat_message("c", 1));
    EXPECT_TRUE(adapter.set_reaction("c", 1, "👍"));
    EXPECT_TRUE(adapter.clear_reactions("c", 1));
}

TEST(TelegramFullApi, SetMyCommandsTruncatesLongDescriptions) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response(true));
    auto adapter = make_adapter(&fake);
    std::string huge(512, 'x');
    EXPECT_TRUE(
        adapter.set_my_commands({{"start", huge}, {"", "ignored"}}));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body["commands"].is_array());
    EXPECT_EQ(body["commands"].size(), 1u);  // empty name filtered
    EXPECT_LE(body["commands"][0]["description"].get<std::string>().size(),
              256u);
}

TEST(TelegramFullApi, SendPhotoBuildsPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 2}}));
    auto adapter = make_adapter(&fake);
    TelegramSendOptions opts;
    opts.message_thread_id = 99;
    auto res = adapter.send_photo("chat", "file_id_abc",
                                  std::string("caption!"), opts);
    EXPECT_TRUE(res.ok);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["photo"], "file_id_abc");
    EXPECT_EQ(body["caption"], "caption!");
    EXPECT_EQ(body["message_thread_id"], 99);
}

TEST(TelegramFullApi, SendPollShape) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 3}}));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(
        adapter.send_poll("c1", "Q?", {"A", "B"}, true, true, 1).ok);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["type"], "quiz");
    EXPECT_EQ(body["correct_option_id"], 1);
    EXPECT_EQ(body["options"].size(), 2u);
}

TEST(TelegramFullApi, SendLocationAndRenderPrompt) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"message_id", 4}}));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(adapter.send_location("c1", 37.7, -122.4).ok);

    nlohmann::json loc_msg = {
        {"location", {{"latitude", 37.7}, {"longitude", -122.4}}}};
    auto rendered = TelegramAdapter::render_location_prompt(loc_msg);
    EXPECT_NE(rendered.find("latitude: 37.7"), std::string::npos);
    EXPECT_NE(rendered.find("longitude: -122.4"), std::string::npos);
}

TEST(TelegramFullApi, GetFileDownloadUrl) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_response({{"file_path", "photos/abc.jpg"}}));
    auto adapter = make_adapter(&fake);
    auto url = adapter.get_file_download_url("fid");
    ASSERT_TRUE(url.has_value());
    EXPECT_NE(url->find("/file/botTKN/photos/abc.jpg"), std::string::npos);
}

TEST(TelegramFullApi, GetUpdatesAdvancesOffset) {
    FakeHttpTransport fake;
    nlohmann::json updates = nlohmann::json::array(
        {{{"update_id", 100}, {"message", {{"text", "hi"}}}},
         {{"update_id", 101}, {"message", {{"text", "yo"}}}}});
    fake.enqueue_response(ok_response(updates));
    auto adapter = make_adapter(&fake);
    auto batch = adapter.get_updates();
    EXPECT_EQ(batch.size(), 2u);
    EXPECT_EQ(adapter.next_update_offset(), 102);
}

TEST(TelegramFullApi, GetUpdatesSkippedDuringFloodWait) {
    FakeHttpTransport fake;
    auto adapter = make_adapter(&fake);
    adapter.set_flood_wait(std::chrono::seconds(10));
    auto batch = adapter.get_updates();
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(fake.requests().empty());
}

TEST(TelegramFullApi, WebhookSetAndDelete) {
    FakeHttpTransport fake;
    for (int i = 0; i < 3; ++i) fake.enqueue_response(ok_response(true));
    auto adapter = make_adapter(&fake);
    EXPECT_TRUE(adapter.set_webhook("https://x/tg",
                                    std::string("secret"),
                                    {"message", "callback_query"}));
    EXPECT_TRUE(adapter.delete_webhook(true));
    EXPECT_TRUE(adapter.answer_callback_query("cbq-1", std::string("ok!"),
                                              false, 10));
}

TEST(TelegramFullApi, ThreadNotFoundRetryWithoutThread) {
    FakeHttpTransport fake;
    // First: thread error.  Second: success.
    fake.enqueue_response(
        {400, R"({"ok":false,"description":"Bad Request: message thread not found"})", {}});
    fake.enqueue_response(ok_response({{"message_id", 5}}));

    TelegramAdapter::Config cfg;
    cfg.bot_token = "T";
    cfg.max_send_retries = 3;
    TelegramAdapter adapter(cfg, &fake);

    TelegramSendOptions opts;
    opts.parse_mode = "";
    opts.message_thread_id = 42;
    auto res = adapter.send_message("c1", "hi", opts);
    EXPECT_TRUE(res.ok);
    ASSERT_EQ(fake.requests().size(), 2u);
    auto body2 = nlohmann::json::parse(fake.requests()[1].body);
    EXPECT_FALSE(body2.contains("message_thread_id"));
}

TEST(TelegramFullApi, ReplyNotFoundRetryWithoutReply) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {400, R"({"ok":false,"description":"Bad Request: message to be replied not found"})", {}});
    fake.enqueue_response(ok_response({{"message_id", 6}}));

    TelegramAdapter::Config cfg;
    cfg.bot_token = "T";
    cfg.max_send_retries = 3;
    TelegramAdapter adapter(cfg, &fake);

    TelegramSendOptions opts;
    opts.parse_mode = "";
    opts.reply_to_message_id = 1234;
    auto res = adapter.send_message("c1", "hi", opts);
    EXPECT_TRUE(res.ok);
    ASSERT_EQ(fake.requests().size(), 2u);
    auto body2 = nlohmann::json::parse(fake.requests()[1].body);
    EXPECT_FALSE(body2.contains("reply_to_message_id"));
}

TEST(TelegramFullApi, ChatMigratedCachesAndRetries) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {400, R"({"ok":false,"description":"Bad Request: migrated","parameters":{"migrate_to_chat_id":-1001}})", {}});
    fake.enqueue_response(ok_response({{"message_id", 7}}));

    TelegramAdapter::Config cfg;
    cfg.bot_token = "T";
    cfg.max_send_retries = 3;
    TelegramAdapter adapter(cfg, &fake);
    TelegramSendOptions opts;
    opts.parse_mode = "";
    auto res = adapter.send_message("oldgroup", "hi", opts);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(fake.requests().size(), 2u);
    auto body2 = nlohmann::json::parse(fake.requests()[1].body);
    EXPECT_EQ(body2["chat_id"], "-1001");
}

TEST(TelegramFullApi, ApprovalRegisterAndTake) {
    FakeHttpTransport fake;
    auto adapter = make_adapter(&fake);
    auto id = adapter.register_approval("session-42");
    auto pulled = adapter.take_approval(id);
    ASSERT_TRUE(pulled.has_value());
    EXPECT_EQ(*pulled, "session-42");
    EXPECT_FALSE(adapter.take_approval(id).has_value());
}
