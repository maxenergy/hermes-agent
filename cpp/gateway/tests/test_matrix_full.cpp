// Full-coverage Matrix adapter tests.  Complements test_matrix_e2ee by
// exercising the REST surface: login, send / edit / redact / react, sync
// parsing, media, rooms, presence, markdown→HTML conversion, rate-limit,
// sync-token persistence, mention detection, etc.
//
// Uses FakeHttpTransport to inject canned Synapse responses — no network.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <hermes/core/path.hpp>

#include "../platforms/matrix.hpp"

using json = nlohmann::json;
using hermes::gateway::platforms::MatrixAdapter;
using hermes::gateway::platforms::MatrixSyncResponse;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;

namespace {

MatrixAdapter::Config make_cfg() {
    MatrixAdapter::Config cfg;
    cfg.homeserver = "https://matrix.example.org";
    cfg.username = "bot";
    cfg.user_id = "@bot:matrix.example.org";
    cfg.password = "pw";
    cfg.access_token = "tok";
    cfg.device_id = "DEV";
    return cfg;
}

HttpTransport::Response make_resp(int code, const std::string& body = "{}") {
    HttpTransport::Response r;
    r.status_code = code;
    r.body = body;
    return r;
}

// Redirect HERMES_HOME to a temp dir so token / thread persistence won't
// pollute the real home during tests.
class HermesHomeGuard {
public:
    HermesHomeGuard() {
        auto tmp = std::filesystem::temp_directory_path() /
                   ("hermes_matrix_test_" + std::to_string(std::rand()));
        std::filesystem::create_directories(tmp);
        tmp_ = tmp.string();
        const char* old = std::getenv("HERMES_HOME");
        if (old) old_.emplace(old);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
    }
    ~HermesHomeGuard() {
        if (old_) setenv("HERMES_HOME", old_->c_str(), 1);
        else unsetenv("HERMES_HOME");
        std::error_code ec;
        std::filesystem::remove_all(tmp_, ec);
    }
    const std::string& path() const { return tmp_; }
private:
    std::string tmp_;
    std::optional<std::string> old_;
};

}  // namespace

// ── Construction / URL helpers ───────────────────────────────────────────

TEST(MatrixFull, HomeserverFromMxid) {
    EXPECT_EQ(MatrixAdapter::homeserver_from_mxid("@alice:example.org"),
              "https://example.org");
    EXPECT_EQ(MatrixAdapter::homeserver_from_mxid("no-colon"), "");
}

TEST(MatrixFull, TrimsTrailingSlashFromHomeserver) {
    MatrixAdapter::Config cfg = make_cfg();
    cfg.homeserver = "https://matrix.example.org/";
    FakeHttpTransport fake;
    MatrixAdapter ad(cfg, &fake);
    EXPECT_EQ(ad.config().homeserver, "https://matrix.example.org");
}

TEST(MatrixFull, MxcToHttpConversion) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    auto url = ad.mxc_to_http("mxc://example.org/abc123");
    EXPECT_EQ(url,
              "https://matrix.example.org/_matrix/client/v1/media/download/example.org/abc123");
    // Non-mxc pass-through.
    EXPECT_EQ(ad.mxc_to_http("https://x/y"), "https://x/y");
}

// ── Payload builders ────────────────────────────────────────────────────

TEST(MatrixFull, BuildMessagePayloadPlain) {
    auto s = MatrixAdapter::build_message_payload("m.text", "hi", "", "", "");
    auto j = json::parse(s);
    EXPECT_EQ(j["msgtype"], "m.text");
    EXPECT_EQ(j["body"], "hi");
    EXPECT_FALSE(j.contains("format"));
    EXPECT_FALSE(j.contains("m.relates_to"));
}

TEST(MatrixFull, BuildMessagePayloadWithHtml) {
    auto s = MatrixAdapter::build_message_payload("m.text", "**b**",
                                                  "<strong>b</strong>", "", "");
    auto j = json::parse(s);
    EXPECT_EQ(j["format"], "org.matrix.custom.html");
    EXPECT_EQ(j["formatted_body"], "<strong>b</strong>");
}

TEST(MatrixFull, BuildMessagePayloadReplyAndThread) {
    auto s = MatrixAdapter::build_message_payload("m.text", "x", "",
                                                  "$reply", "$thread");
    auto j = json::parse(s);
    EXPECT_EQ(j["m.relates_to"]["rel_type"], "m.thread");
    EXPECT_EQ(j["m.relates_to"]["event_id"], "$thread");
    EXPECT_EQ(j["m.relates_to"]["m.in_reply_to"]["event_id"], "$reply");
    EXPECT_TRUE(j["m.relates_to"]["is_falling_back"].get<bool>());
}

TEST(MatrixFull, BuildEditPayloadReplace) {
    auto s = MatrixAdapter::build_edit_payload("$orig", "new body",
                                               "<p>new</p>");
    auto j = json::parse(s);
    EXPECT_EQ(j["body"], "* new body");
    EXPECT_EQ(j["m.relates_to"]["rel_type"], "m.replace");
    EXPECT_EQ(j["m.relates_to"]["event_id"], "$orig");
    EXPECT_EQ(j["m.new_content"]["body"], "new body");
    EXPECT_EQ(j["m.new_content"]["format"], "org.matrix.custom.html");
}

TEST(MatrixFull, BuildReactionPayloadAnnotation) {
    auto s = MatrixAdapter::build_reaction_payload("$ev", "👍");
    auto j = json::parse(s);
    EXPECT_EQ(j["m.relates_to"]["rel_type"], "m.annotation");
    EXPECT_EQ(j["m.relates_to"]["event_id"], "$ev");
    EXPECT_EQ(j["m.relates_to"]["key"], "👍");
}

// ── Chunking ─────────────────────────────────────────────────────────────

TEST(MatrixFull, ChunkMessageUnderLimit) {
    auto chunks = MatrixAdapter::chunk_message("hello", 100);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "hello");
}

TEST(MatrixFull, ChunkMessageSplitsOnWordBoundary) {
    std::string body = std::string(30, 'a') + " " + std::string(30, 'b');
    auto chunks = MatrixAdapter::chunk_message(body, 32);
    ASSERT_GE(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], std::string(30, 'a') + " ");
}

// ── Markdown → HTML ─────────────────────────────────────────────────────

TEST(MatrixFull, MarkdownBold) {
    auto h = MatrixAdapter::markdown_to_html("this is **bold**");
    EXPECT_NE(h.find("<strong>bold</strong>"), std::string::npos);
}

TEST(MatrixFull, MarkdownInlineCodeProtectedFromEscape) {
    auto h = MatrixAdapter::markdown_to_html("use `x < y`");
    EXPECT_NE(h.find("<code>x &lt; y</code>"), std::string::npos);
}

TEST(MatrixFull, MarkdownFencedCodeBlock) {
    auto h = MatrixAdapter::markdown_to_html("```cpp\nint x = 1;\n```");
    EXPECT_NE(h.find("<pre><code class=\"language-cpp\">"), std::string::npos);
    EXPECT_NE(h.find("int x = 1;"), std::string::npos);
}

TEST(MatrixFull, MarkdownLinkSanitisesJavascriptScheme) {
    auto h = MatrixAdapter::markdown_to_html("[click](javascript:evil)");
    // javascript scheme stripped → href empty.
    EXPECT_EQ(h.find("javascript"), std::string::npos);
}

TEST(MatrixFull, MarkdownHeader) {
    auto h = MatrixAdapter::markdown_to_html("# Title\n");
    EXPECT_NE(h.find("<h1>Title</h1>"), std::string::npos);
}

TEST(MatrixFull, MarkdownUnorderedList) {
    auto h = MatrixAdapter::markdown_to_html("- a\n- b\n");
    EXPECT_NE(h.find("<ul>"), std::string::npos);
    EXPECT_NE(h.find("<li>a</li>"), std::string::npos);
}

TEST(MatrixFull, HtmlEscapeBasic) {
    EXPECT_EQ(MatrixAdapter::html_escape("a<b>c&d"), "a&lt;b&gt;c&amp;d");
}

TEST(MatrixFull, SanitizeLinkBlocksDangerousSchemes) {
    EXPECT_EQ(MatrixAdapter::sanitize_link_url("javascript:alert(1)"), "");
    EXPECT_EQ(MatrixAdapter::sanitize_link_url("data:text/html,x"), "");
    auto safe = MatrixAdapter::sanitize_link_url("https://example.com/\"x");
    EXPECT_NE(safe.find("&quot;"), std::string::npos);
}

// ── Mention detection ────────────────────────────────────────────────────

TEST(MatrixFull, IsBotMentionedByFullMxid) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.is_bot_mentioned("hi @bot:matrix.example.org"));
    EXPECT_FALSE(ad.is_bot_mentioned("plain text"));
}

TEST(MatrixFull, IsBotMentionedByLocalpart) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.is_bot_mentioned("hey bot can you"));
}

TEST(MatrixFull, IsBotMentionedInFormattedBody) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.is_bot_mentioned(
        "placeholder",
        "<a href=\"https://matrix.to/#/@bot:matrix.example.org\">bot</a>"));
}

TEST(MatrixFull, StripMentionRemovesMxid) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    auto s = ad.strip_mention("hey @bot:matrix.example.org how are you");
    EXPECT_EQ(s.find("@bot:matrix.example.org"), std::string::npos);
    EXPECT_NE(s.find("how are you"), std::string::npos);
}

// ── Login / auth ─────────────────────────────────────────────────────────

TEST(MatrixFull, LoginPasswordExtractsToken) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"access_token":"NEW",
        "user_id":"@bot:matrix.example.org","device_id":"DEV"})"));
    auto cfg = make_cfg();
    cfg.access_token.clear();
    MatrixAdapter ad(cfg, &fake);
    auto token = ad.login_password();
    EXPECT_EQ(token, "NEW");
    EXPECT_EQ(ad.access_token(), "NEW");
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/_matrix/client/v3/login"),
              std::string::npos);
}

TEST(MatrixFull, LoginSsoTokenExchange) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200,
        R"({"access_token":"SSO","user_id":"@x:y","device_id":"D"})"));
    auto cfg = make_cfg();
    cfg.access_token.clear();
    MatrixAdapter ad(cfg, &fake);
    auto token = ad.login_sso_token("login-token");
    EXPECT_EQ(token, "SSO");
}

TEST(MatrixFull, SsoRedirectUrl) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    auto url = ad.sso_redirect_url("https://app.example.com/callback");
    EXPECT_NE(url.find("/login/sso/redirect"), std::string::npos);
    EXPECT_NE(url.find("redirectUrl="), std::string::npos);
}

TEST(MatrixFull, RefreshAccessTokenUpdatesToken) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200,
        R"({"access_token":"REFRESHED","refresh_token":"NEWREFRESH"})"));
    auto cfg = make_cfg();
    cfg.refresh_token = "r1";
    MatrixAdapter ad(cfg, &fake);
    EXPECT_TRUE(ad.refresh_access_token());
    EXPECT_EQ(ad.access_token(), "REFRESHED");
}

TEST(MatrixFull, RefreshFailsWithoutRefreshToken) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_FALSE(ad.refresh_access_token());
}

TEST(MatrixFull, DiscoverHomeserverRewritesBaseUrl) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200,
        R"({"m.homeserver":{"base_url":"https://real.example.org/"}})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto hs = ad.discover_homeserver();
    EXPECT_EQ(hs, "https://real.example.org");
}

// ── Send / edit / redact / react ─────────────────────────────────────────

TEST(MatrixFull, SendTextPostsEvent) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$abc"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto result = ad.send_text("!room:x", "hello");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.message_id, "$abc");
    ASSERT_EQ(fake.requests().size(), 1u);
    const auto& req = fake.requests()[0];
    EXPECT_NE(req.url.find("/send/m.room.message/"), std::string::npos);
    auto body = json::parse(req.body);
    EXPECT_EQ(body["msgtype"], "m.text");
    EXPECT_EQ(body["body"], "hello");
}

TEST(MatrixFull, SendMarkdownIncludesFormattedBody) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$m"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto result = ad.send_markdown("!r:x", "a **bold** word");
    EXPECT_TRUE(result.success);
    ASSERT_EQ(fake.requests().size(), 1u);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["format"], "org.matrix.custom.html");
    EXPECT_NE(body["formatted_body"].get<std::string>().find("<strong>bold</strong>"),
              std::string::npos);
}

TEST(MatrixFull, SendRespectsThreadRelation) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$e"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_text("!r:x", "reply", "$rep", "$thr");
    EXPECT_TRUE(r.success);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["m.relates_to"]["rel_type"], "m.thread");
    EXPECT_EQ(body["m.relates_to"]["event_id"], "$thr");
    // Thread should be recorded as participated.
    EXPECT_TRUE(ad.is_thread_participated("$thr"));
}

TEST(MatrixFull, SendFailsWithoutToken) {
    FakeHttpTransport fake;
    auto cfg = make_cfg();
    cfg.access_token.clear();
    MatrixAdapter ad(cfg, &fake);
    auto r = ad.send_text("!r:x", "hi");
    EXPECT_FALSE(r.success);
}

TEST(MatrixFull, SendPropagatesHttpError) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(500, "oops"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_text("!r:x", "hi");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("500"), std::string::npos);
}

TEST(MatrixFull, EditMessageUsesReplaceRelation) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$e2"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.edit_message("!r:x", "$orig", "new body");
    EXPECT_TRUE(r.success);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["m.relates_to"]["rel_type"], "m.replace");
    EXPECT_EQ(body["m.relates_to"]["event_id"], "$orig");
}

TEST(MatrixFull, RedactMessageIssuesRedactRequest) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$redact"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.redact_message("!r:x", "$ev", "spam"));
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/redact/"), std::string::npos);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["reason"], "spam");
}

TEST(MatrixFull, SendReactionPostsAnnotation) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$rxn"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_reaction("!r:x", "$target", "👍");
    EXPECT_TRUE(r.success);
    EXPECT_NE(fake.requests()[0].url.find("/send/m.reaction/"), std::string::npos);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["m.relates_to"]["key"], "👍");
}

TEST(MatrixFull, SendEmoteUsesEmoteMsgtype) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$e"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_emote("!r:x", "waves");
    EXPECT_TRUE(r.success);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["msgtype"], "m.emote");
}

TEST(MatrixFull, SendNoticeUsesNoticeMsgtype) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$e"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_notice("!r:x", "FYI");
    EXPECT_TRUE(r.success);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["msgtype"], "m.notice");
}

// ── Media upload / send ─────────────────────────────────────────────────

TEST(MatrixFull, UploadMediaReturnsContentUri) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200,
        R"({"content_uri":"mxc://matrix.example.org/abc"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto mxc = ad.upload_media("image/png", "a.png", "bytes-here");
    EXPECT_EQ(mxc, "mxc://matrix.example.org/abc");
    EXPECT_NE(fake.requests()[0].url.find("/_matrix/media/v3/upload"),
              std::string::npos);
}

TEST(MatrixFull, SendMediaIncludesMxcAndInfo) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"event_id":"$img"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_media("!r:x", "m.image", "mxc://x/y", "a.png",
                           "image/png", 1024, "nice", "$reply");
    EXPECT_TRUE(r.success);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["msgtype"], "m.image");
    EXPECT_EQ(body["url"], "mxc://x/y");
    EXPECT_EQ(body["info"]["mimetype"], "image/png");
    EXPECT_EQ(body["info"]["size"], 1024);
    EXPECT_EQ(body["m.relates_to"]["m.in_reply_to"]["event_id"], "$reply");
}

// ── Room ops ─────────────────────────────────────────────────────────────

TEST(MatrixFull, CreateRoomReturnsRoomId) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"room_id":"!new:x"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto rid = ad.create_room("My Room", "A topic", {"@alice:x"}, false);
    EXPECT_EQ(rid, "!new:x");
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["name"], "My Room");
    EXPECT_EQ(body["topic"], "A topic");
    EXPECT_EQ(body["invite"][0], "@alice:x");
}

TEST(MatrixFull, EnsureDmRoomCachesResult) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"room_id":"!dm:x"})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto rid = ad.ensure_dm_room("@alice:x");
    EXPECT_EQ(rid, "!dm:x");
    // Second call should not hit HTTP again.
    auto rid2 = ad.ensure_dm_room("@alice:x");
    EXPECT_EQ(rid2, "!dm:x");
    EXPECT_EQ(fake.requests().size(), 1u);
}

TEST(MatrixFull, InviteKickBanReturnTrueOn200) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    fake.enqueue_response(make_resp(200, "{}"));
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.invite_user("!r", "@a:x"));
    EXPECT_TRUE(ad.kick_user("!r", "@a:x", "bye"));
    EXPECT_TRUE(ad.ban_user("!r", "@a:x"));
    ASSERT_EQ(fake.requests().size(), 3u);
    EXPECT_NE(fake.requests()[0].url.find("/invite"), std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("/kick"), std::string::npos);
    EXPECT_NE(fake.requests()[2].url.find("/ban"), std::string::npos);
}

TEST(MatrixFull, JoinAndLeaveRoom) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.join_room("!r:x"));
    EXPECT_TRUE(ad.leave_room("!r:x"));
    EXPECT_NE(fake.requests()[0].url.find("/join/"), std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("/leave"), std::string::npos);
}

TEST(MatrixFull, SetRoomNameAndTopic) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.set_room_name("!r:x", "hi"));
    EXPECT_TRUE(ad.set_room_topic("!r:x", "about"));
}

TEST(MatrixFull, SendReadReceipt) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.send_read_receipt("!r:x", "$e"));
    EXPECT_NE(fake.requests()[0].url.find("/receipt/m.read/"), std::string::npos);
}

TEST(MatrixFull, SendTypingPostsIndicator) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    ad.send_typing("!r:x");
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/typing/"), std::string::npos);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_TRUE(body["typing"].get<bool>());
}

// ── Presence ─────────────────────────────────────────────────────────────

TEST(MatrixFull, SetPresenceRejectsInvalidState) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_FALSE(ad.set_presence("xyzzy"));
    EXPECT_EQ(fake.requests().size(), 0u);
}

TEST(MatrixFull, SetPresenceOnlineSendsPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.set_presence("online", "working"));
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["presence"], "online");
    EXPECT_EQ(body["status_msg"], "working");
}

// ── Sync ─────────────────────────────────────────────────────────────────

TEST(MatrixFull, ParseSyncResponseExtractsTimelineEvents) {
    std::string body = R"({
        "next_batch":"s2",
        "rooms":{"join":{"!r:x":{
            "timeline":{"events":[
                {"event_id":"$1","sender":"@a:x","type":"m.room.message",
                 "origin_server_ts":123,
                 "content":{"msgtype":"m.text","body":"hi"}},
                {"event_id":"$2","sender":"@a:x","type":"m.reaction",
                 "content":{"m.relates_to":{"rel_type":"m.annotation",
                     "event_id":"$1","key":"👍"}}}
            ]},
            "ephemeral":{"events":[
                {"type":"m.typing","content":{"user_ids":["@a:x"]}}
            ]}
        }},
        "invite":{"!inv:x":{}}}
    })";
    auto r = MatrixAdapter::parse_sync_response(body);
    EXPECT_EQ(r.next_batch, "s2");
    ASSERT_EQ(r.room_events["!r:x"].size(), 2u);
    EXPECT_EQ(r.room_events["!r:x"][0].body, "hi");
    EXPECT_EQ(r.room_events["!r:x"][1].reaction_key, "👍");
    EXPECT_EQ(r.typing["!r:x"].size(), 1u);
    EXPECT_EQ(r.invites.count("!inv:x"), 1u);
}

TEST(MatrixFull, ParseSyncResponseExtractsReplyRelation) {
    std::string body = R"({
        "next_batch":"s3",
        "rooms":{"join":{"!r:x":{"timeline":{"events":[
            {"event_id":"$r","type":"m.room.message","sender":"@a:x",
             "content":{"msgtype":"m.text","body":"re",
                "m.relates_to":{"m.in_reply_to":{"event_id":"$orig"}}}}
        ]}}}}
    })";
    auto r = MatrixAdapter::parse_sync_response(body);
    EXPECT_EQ(r.room_events["!r:x"][0].in_reply_to_event_id, "$orig");
}

TEST(MatrixFull, SyncOnceUpdatesNextBatch) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200,
        R"({"next_batch":"tok1","rooms":{}})"));
    MatrixAdapter ad(make_cfg(), &fake);
    MatrixSyncResponse r;
    EXPECT_TRUE(ad.sync_once(r));
    EXPECT_EQ(ad.next_batch(), "tok1");
}

// ── Rate limit ───────────────────────────────────────────────────────────

TEST(MatrixFull, RateLimitResponseRecordsRetryAfter) {
    FakeHttpTransport fake;
    // 429 first, 429 again (retry exhausted) — both consumed.
    fake.enqueue_response(make_resp(429,
        R"({"errcode":"M_LIMIT_EXCEEDED","retry_after_ms":500})"));
    fake.enqueue_response(make_resp(429,
        R"({"errcode":"M_LIMIT_EXCEEDED","retry_after_ms":500})"));
    fake.enqueue_response(make_resp(429,
        R"({"errcode":"M_LIMIT_EXCEEDED","retry_after_ms":500})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto r = ad.send_text("!r:x", "hi");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(ad.rate_limit_state().retry_after_ms, 500);
    EXPECT_GT(ad.rate_limit_state().hit_count, 0);
}

// ── Fetch history ───────────────────────────────────────────────────────

TEST(MatrixFull, FetchRoomHistoryReversesChunk) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"chunk":[
        {"event_id":"$b","sender":"@a:x","type":"m.room.message",
         "origin_server_ts":2,"content":{"msgtype":"m.text","body":"B"}},
        {"event_id":"$a","sender":"@a:x","type":"m.room.message",
         "origin_server_ts":1,"content":{"msgtype":"m.text","body":"A"}}
    ]})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto msgs = ad.fetch_room_history("!r:x", 10);
    ASSERT_EQ(msgs.size(), 2u);
    // Reversed to chronological order.
    EXPECT_EQ(msgs[0].body, "A");
    EXPECT_EQ(msgs[1].body, "B");
}

// ── Duplicate suppression ───────────────────────────────────────────────

TEST(MatrixFull, ObserveEventDedupesRepeats) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.observe_event("$1"));
    EXPECT_FALSE(ad.observe_event("$1"));
    EXPECT_TRUE(ad.observe_event("$2"));
}

// ── Thread persistence ──────────────────────────────────────────────────

TEST(MatrixFull, ParticipatedThreadsRoundTripThroughDisk) {
    HermesHomeGuard guard;
    FakeHttpTransport fake;
    {
        MatrixAdapter ad(make_cfg(), &fake);
        ad.track_thread("$thr1");
        ad.track_thread("$thr2");
        EXPECT_TRUE(ad.save_participated_threads());
    }
    MatrixAdapter ad2(make_cfg(), &fake);
    EXPECT_TRUE(ad2.load_participated_threads());
    EXPECT_TRUE(ad2.is_thread_participated("$thr1"));
    EXPECT_TRUE(ad2.is_thread_participated("$thr2"));
}

// ── Sync-token persistence ──────────────────────────────────────────────

TEST(MatrixFull, SyncTokenLoadAfterSave) {
    HermesHomeGuard guard;
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"next_batch":"persisted","rooms":{}})"));
    {
        MatrixAdapter ad(make_cfg(), &fake);
        MatrixSyncResponse r;
        ASSERT_TRUE(ad.sync_once(r));
        EXPECT_EQ(ad.next_batch(), "persisted");
        ad.disconnect();  // triggers save_sync_token
    }
    MatrixAdapter ad2(make_cfg(), &fake);
    EXPECT_TRUE(ad2.load_sync_token());
    EXPECT_EQ(ad2.next_batch(), "persisted");
}

// ── Transaction ID monotonicity ─────────────────────────────────────────

TEST(MatrixFull, TxnIdsAreUnique) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    auto a = ad.next_txn_id();
    auto b = ad.next_txn_id();
    auto c = ad.next_txn_id();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

// ── Cross-signing + device keys ─────────────────────────────────────────

TEST(MatrixFull, ClaimOneTimeKeysPostsBundle) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"one_time_keys":{}})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto body = ad.claim_one_time_keys({{"@a:x", "D1"}, {"@b:x", "D2"}});
    EXPECT_FALSE(body.empty());
    auto sent = json::parse(fake.requests()[0].body);
    EXPECT_EQ(sent["one_time_keys"]["@a:x"]["D1"], "signed_curve25519");
}

TEST(MatrixFull, QueryDeviceKeysSendsUserList) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, R"({"device_keys":{}})"));
    MatrixAdapter ad(make_cfg(), &fake);
    auto body = ad.query_device_keys({"@a:x", "@b:x"});
    EXPECT_FALSE(body.empty());
    auto sent = json::parse(fake.requests()[0].body);
    EXPECT_TRUE(sent["device_keys"].contains("@a:x"));
}

TEST(MatrixFull, EnableRoomEncryptionPostsState) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(200, "{}"));
    MatrixAdapter ad(make_cfg(), &fake);
    EXPECT_TRUE(ad.enable_room_encryption("!r:x"));
    EXPECT_NE(fake.requests()[0].url.find("m.room.encryption"), std::string::npos);
    auto body = json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["algorithm"], "m.megolm.v1.aes-sha2");
}

// ── Fallback behaviour without libolm ────────────────────────────────────

TEST(MatrixFull, EncryptPassThroughWhenNoSession) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    std::string out;
    EXPECT_TRUE(ad.encrypt_room_message("!r:x", "plain", out));
    EXPECT_EQ(out, "plain");
}

TEST(MatrixFull, DecryptPassThroughWhenNoSession) {
    FakeHttpTransport fake;
    MatrixAdapter ad(make_cfg(), &fake);
    auto out = ad.decrypt_room_message("!r:x", "ct");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "ct");
}
