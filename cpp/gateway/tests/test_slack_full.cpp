// Full-surface tests for SlackAdapter — Block Kit builders, Web API
// wrappers, mrkdwn formatter, signature verification and 429 retries.
#include <gtest/gtest.h>

#include "../platforms/slack.hpp"

#include <chrono>
#include <ctime>
#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

using hermes::gateway::platforms::SlackAdapter;
using hermes::llm::FakeHttpTransport;

namespace {

SlackAdapter::Config base_cfg() {
    SlackAdapter::Config cfg;
    cfg.bot_token = "xoxb-test";
    cfg.signing_secret = "sek";
    cfg.max_retries_on_429 = 2;
    cfg.retry_after_override_ms = -1;  // no sleep during tests
    return cfg;
}

}  // namespace

// ----- Block Kit builders -------------------------------------------------

TEST(SlackFull, SectionBlockBuildsMrkdwn) {
    auto b = SlackAdapter::section_block("hello *world*", "b1");
    EXPECT_EQ(b["type"], "section");
    EXPECT_EQ(b["text"]["type"], "mrkdwn");
    EXPECT_EQ(b["text"]["text"], "hello *world*");
    EXPECT_EQ(b["block_id"], "b1");
}

TEST(SlackFull, DividerBlockShape) {
    auto b = SlackAdapter::divider_block();
    EXPECT_EQ(b["type"], "divider");
}

TEST(SlackFull, HeaderAndContextAndImage) {
    auto h = SlackAdapter::header_block("Title");
    EXPECT_EQ(h["type"], "header");
    EXPECT_EQ(h["text"]["type"], "plain_text");

    auto c = SlackAdapter::context_block({"a", "b"});
    EXPECT_EQ(c["type"], "context");
    EXPECT_EQ(c["elements"].size(), 2u);

    auto i = SlackAdapter::image_block("http://img", "alt", "T");
    EXPECT_EQ(i["type"], "image");
    EXPECT_EQ(i["alt_text"], "alt");
    EXPECT_EQ(i["title"]["text"], "T");
}

TEST(SlackFull, InputAndActionsBlock) {
    auto input = SlackAdapter::plain_text_input_element("a1", true);
    auto ib = SlackAdapter::input_block("Label", input, "bid", true);
    EXPECT_EQ(ib["type"], "input");
    EXPECT_EQ(ib["element"]["action_id"], "a1");
    EXPECT_TRUE(ib["optional"].get<bool>());

    auto btn = SlackAdapter::button_element("Go", "go_id", "v", "primary");
    auto ab = SlackAdapter::actions_block({btn}, "actblock");
    EXPECT_EQ(ab["type"], "actions");
    EXPECT_EQ(ab["elements"][0]["style"], "primary");
    EXPECT_EQ(ab["block_id"], "actblock");
}

TEST(SlackFull, SelectAndDatepickerElements) {
    auto sel = SlackAdapter::static_select_element(
        "a", "Pick", {{"One", "1"}, {"Two", "2"}});
    EXPECT_EQ(sel["type"], "static_select");
    EXPECT_EQ(sel["options"].size(), 2u);
    EXPECT_EQ(sel["options"][1]["value"], "2");

    auto dp = SlackAdapter::datepicker_element("d", "pick", "2026-01-01");
    EXPECT_EQ(dp["type"], "datepicker");
    EXPECT_EQ(dp["initial_date"], "2026-01-01");

    EXPECT_EQ(SlackAdapter::users_select_element("u", "p")["type"],
              "users_select");
    EXPECT_EQ(SlackAdapter::channels_select_element("c", "p")["type"],
              "channels_select");
}

TEST(SlackFull, ModalAndHomeViewEnvelopes) {
    auto m = SlackAdapter::modal_view(
        "T", {SlackAdapter::divider_block()}, "Submit", "Close", "cb", "priv");
    EXPECT_EQ(m["type"], "modal");
    EXPECT_EQ(m["title"]["text"], "T");
    EXPECT_EQ(m["submit"]["text"], "Submit");
    EXPECT_EQ(m["callback_id"], "cb");
    EXPECT_EQ(m["private_metadata"], "priv");

    auto h = SlackAdapter::home_view({SlackAdapter::divider_block()});
    EXPECT_EQ(h["type"], "home");
    EXPECT_EQ(h["blocks"].size(), 1u);
}

// ----- Messaging ----------------------------------------------------------

TEST(SlackFull, SendBlocksPostsPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.send_blocks(
        "C1", {SlackAdapter::section_block("hi")}, "fallback", "T1"));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["channel"], "C1");
    EXPECT_EQ(body["text"], "fallback");
    EXPECT_EQ(body["thread_ts"], "T1");
    ASSERT_TRUE(body.contains("blocks"));
    EXPECT_EQ(body["blocks"][0]["type"], "section");
}

TEST(SlackFull, SendEphemeralRoutesToCorrectEndpoint) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.send_ephemeral("C1", "U1", "hi"));
    EXPECT_NE(fake.requests()[0].url.find("chat.postEphemeral"),
              std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["user"], "U1");
}

TEST(SlackFull, UpdateAndDeleteMessage) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.update_message("C", "1.1", "new"));
    EXPECT_NE(fake.requests()[0].url.find("chat.update"), std::string::npos);
    EXPECT_TRUE(a.delete_message("C", "1.1"));
    EXPECT_NE(fake.requests()[1].url.find("chat.delete"), std::string::npos);
}

// ----- Reactions / pins / stars ------------------------------------------

TEST(SlackFull, ReactionsAddRemoveAndGet) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200,
                           R"({"ok":true,"message":{"reactions":[{"name":"+1"}]}})",
                           {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.add_reaction("C", "1.1", "+1"));
    EXPECT_TRUE(a.remove_reaction("C", "1.1", "+1"));
    auto got = a.get_reactions("C", "1.1");
    EXPECT_TRUE(got.value("ok", false));
}

TEST(SlackFull, PinUnpinStar) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.pin_message("C", "1"));
    EXPECT_TRUE(a.unpin_message("C", "1"));
    EXPECT_TRUE(a.add_star("C", "1"));
    EXPECT_NE(fake.requests()[0].url.find("pins.add"), std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("pins.remove"), std::string::npos);
    EXPECT_NE(fake.requests()[2].url.find("stars.add"), std::string::npos);
}

// ----- Conversations ------------------------------------------------------

TEST(SlackFull, OpenDmReturnsChannelId) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200, R"({"ok":true,"channel":{"id":"D99"}})", {}});
    SlackAdapter a(base_cfg(), &fake);
    auto id = a.open_dm("U1");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "D99");
}

TEST(SlackFull, OpenMpimJoinsUsers) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200, R"({"ok":true,"channel":{"id":"G42"}})", {}});
    SlackAdapter a(base_cfg(), &fake);
    auto id = a.open_mpim({"U1", "U2", "U3"});
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "G42");
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["users"], "U1,U2,U3");
}

TEST(SlackFull, ChannelInfoAndMembersAndHistory) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200, R"({"ok":true,"channel":{"id":"C1"}})", {}});
    fake.enqueue_response(
        {200, R"({"ok":true,"members":["U1","U2"]})", {}});
    fake.enqueue_response(
        {200, R"({"ok":true,"messages":[{"ts":"1"}]})", {}});
    fake.enqueue_response(
        {200, R"({"ok":true,"channels":[]})", {}});
    SlackAdapter a(base_cfg(), &fake);
    auto info = a.get_channel_info("C1");
    EXPECT_TRUE(info.value("ok", false));
    auto members = a.get_channel_members("C1", 50);
    EXPECT_EQ(members["members"].size(), 2u);
    auto hist = a.get_channel_history("C1", 10, "cur1");
    EXPECT_TRUE(hist.value("ok", false));
    auto list = a.list_channels();
    EXPECT_TRUE(list.value("ok", false));

    // URL encoding check: conversations.history includes cursor.
    EXPECT_NE(fake.requests()[2].url.find("cursor=cur1"), std::string::npos);
}

// ----- Users -------------------------------------------------------------

TEST(SlackFull, UserInfoAndLookupByEmailAndPresence) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200, R"({"ok":true,"user":{"id":"U1","name":"n"}})", {}});
    fake.enqueue_response(
        {200, R"({"ok":true,"user":{"id":"U9"}})", {}});
    fake.enqueue_response({200, R"({"ok":true,"presence":"active"})", {}});
    SlackAdapter a(base_cfg(), &fake);
    auto info = a.get_user_info("U1");
    EXPECT_EQ(info["user"]["id"], "U1");
    auto id = a.lookup_user_by_email("x@y.com");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "U9");
    auto pr = a.get_user_presence("U1");
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(*pr, "active");
}

// ----- Views -------------------------------------------------------------

TEST(SlackFull, ViewsOpenPushUpdatePublish) {
    FakeHttpTransport fake;
    for (int i = 0; i < 4; ++i) {
        fake.enqueue_response({200, R"({"ok":true})", {}});
    }
    SlackAdapter a(base_cfg(), &fake);
    auto v = SlackAdapter::modal_view("T", {});
    EXPECT_TRUE(a.views_open("trig", v));
    EXPECT_TRUE(a.views_push("trig", v));
    EXPECT_TRUE(a.views_update("V1", v));
    EXPECT_TRUE(a.views_publish("U1", SlackAdapter::home_view({}), "h"));
    EXPECT_NE(fake.requests()[0].url.find("views.open"), std::string::npos);
    EXPECT_NE(fake.requests()[3].url.find("views.publish"),
              std::string::npos);
    auto pub_body = nlohmann::json::parse(fake.requests()[3].body);
    EXPECT_EQ(pub_body["hash"], "h");
}

// ----- Files (upload_v2 flow) -------------------------------------------

TEST(SlackFull, GetUploadUrlExternalAndComplete) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"ok":true,"file_id":"F1","upload_url":"https://up/x"})",
         {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    auto r = a.get_upload_url_external("n.txt", 42);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.file_id, "F1");
    EXPECT_EQ(r.upload_url, "https://up/x");
    EXPECT_NE(fake.requests()[0].url.find("files.getUploadURLExternal"),
              std::string::npos);
    EXPECT_NE(fake.requests()[0].url.find("length=42"), std::string::npos);

    EXPECT_TRUE(a.complete_upload_external("F1", "title", "C1", "note", "T1"));
    auto body = nlohmann::json::parse(fake.requests()[1].body);
    EXPECT_EQ(body["files"][0]["id"], "F1");
    EXPECT_EQ(body["channel_id"], "C1");
    EXPECT_EQ(body["thread_ts"], "T1");
}

// ----- mrkdwn formatting ------------------------------------------------

TEST(SlackFull, FormatMessageHeaderBold) {
    auto out = SlackAdapter::format_message("# Hello\n**bold**");
    EXPECT_NE(out.find("*Hello*"), std::string::npos);
    EXPECT_NE(out.find("*bold*"), std::string::npos);
}

TEST(SlackFull, FormatMessageLinkAndStrike) {
    auto out = SlackAdapter::format_message("[label](http://x) ~~gone~~");
    EXPECT_NE(out.find("<http://x|label>"), std::string::npos);
    EXPECT_NE(out.find("~gone~"), std::string::npos);
}

TEST(SlackFull, FormatMessagePreservesFencedCode) {
    std::string in = "before\n```\n**not bold**\n```\nafter";
    auto out = SlackAdapter::format_message(in);
    EXPECT_NE(out.find("**not bold**"), std::string::npos);
}

// ----- Signatures --------------------------------------------------------

TEST(SlackFull, VerifyEventsApiSignatureAcceptsFreshPayload) {
    auto ts = std::to_string(std::time(nullptr));
    std::string body = R"({"type":"url_verification"})";
    auto sig = SlackAdapter::compute_slack_signature("sek", ts, body);
    EXPECT_TRUE(
        SlackAdapter::verify_events_api_signature("sek", ts, body, sig));
}

TEST(SlackFull, VerifyEventsApiSignatureRejectsStaleTimestamp) {
    auto ts = std::to_string(std::time(nullptr) - 10'000);
    std::string body = "{}";
    auto sig = SlackAdapter::compute_slack_signature("sek", ts, body);
    EXPECT_FALSE(
        SlackAdapter::verify_events_api_signature("sek", ts, body, sig));
}

TEST(SlackFull, VerifyEventsApiSignatureRejectsBadSig) {
    auto ts = std::to_string(std::time(nullptr));
    EXPECT_FALSE(SlackAdapter::verify_events_api_signature(
        "sek", ts, "{}", "v0=deadbeef"));
}

// ----- Retry / rate-limit -----------------------------------------------

TEST(SlackFull, RateLimit429RetriesThenSucceeds) {
    FakeHttpTransport fake;
    // First a 429, then 200 OK.
    fake.enqueue_response({429, "", {{"retry-after", "1"}}});
    fake.enqueue_response({200, R"({"ok":true})", {}});
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_TRUE(a.send("C", "hi"));
    EXPECT_EQ(a.last_retry_count(), 1);
    EXPECT_EQ(fake.requests().size(), 2u);
}

TEST(SlackFull, RateLimit429GivesUpAfterMaxRetries) {
    FakeHttpTransport fake;
    for (int i = 0; i < 4; ++i) {
        fake.enqueue_response({429, "", {{"retry-after", "1"}}});
    }
    SlackAdapter a(base_cfg(), &fake);
    EXPECT_FALSE(a.send("C", "hi"));
    // max_retries_on_429=2 → 1 initial + 2 retries = 3 attempts.
    EXPECT_EQ(fake.requests().size(), 3u);
    EXPECT_EQ(a.last_retry_count(), 2);
}

// ----- Enterprise Grid parsing ------------------------------------------

TEST(SlackFull, ParseGridIdentityTopLevel) {
    nlohmann::json env = {{"team_id", "T1"}, {"enterprise_id", "E1"}};
    auto g = SlackAdapter::parse_grid_identity(env);
    EXPECT_EQ(g.team_id, "T1");
    EXPECT_EQ(g.enterprise_id, "E1");
}

TEST(SlackFull, ParseGridIdentityFromAuthorizations) {
    nlohmann::json env = {
        {"authorizations",
         nlohmann::json::array({{{"team_id", "T2"},
                                 {"enterprise_id", "E2"},
                                 {"is_enterprise_install", true}}})}};
    auto g = SlackAdapter::parse_grid_identity(env);
    EXPECT_EQ(g.team_id, "T2");
    EXPECT_EQ(g.enterprise_id, "E2");
    EXPECT_TRUE(g.is_enterprise_install);
}

// ----- Ignorable subtypes ------------------------------------------------

TEST(SlackFull, IgnorableSubtypeDetected) {
    nlohmann::json ev = {{"subtype", "bot_message"}};
    EXPECT_TRUE(SlackAdapter::is_ignorable_subtype(ev));
    nlohmann::json ev2 = {{"subtype", "channel_join"}};
    EXPECT_TRUE(SlackAdapter::is_ignorable_subtype(ev2));
    nlohmann::json ev3 = {{"text", "hi"}};
    EXPECT_FALSE(SlackAdapter::is_ignorable_subtype(ev3));
}

// ----- Slash form parse --------------------------------------------------

TEST(SlackFull, ParseSlashFormDecodes) {
    std::string body =
        "token=X&team_id=T1&channel_id=C1&user_id=U1"
        "&command=%2Fhermes&text=hello+world"
        "&trigger_id=trig";
    auto m = SlackAdapter::parse_slash_form(body);
    EXPECT_EQ(m["token"], "X");
    EXPECT_EQ(m["team_id"], "T1");
    EXPECT_EQ(m["command"], "/hermes");
    EXPECT_EQ(m["text"], "hello world");
    EXPECT_EQ(m["trigger_id"], "trig");
}
