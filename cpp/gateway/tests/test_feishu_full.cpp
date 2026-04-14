// Unit tests for the full-depth FeishuAdapter port.
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

#include "../platforms/feishu.hpp"

using hermes::gateway::MessageEvent;
using hermes::gateway::platforms::build_markdown_post_payload;
using hermes::gateway::platforms::classify_feishu_error;
using hermes::gateway::platforms::coerce_int;
using hermes::gateway::platforms::coerce_required_int;
using hermes::gateway::platforms::escape_markdown_text;
using hermes::gateway::platforms::FeishuAdapter;
using hermes::gateway::platforms::FeishuAnomalyTracker;
using hermes::gateway::platforms::FeishuConnectionMode;
using hermes::gateway::platforms::FeishuDedupCache;
using hermes::gateway::platforms::FeishuErrorKind;
using hermes::gateway::platforms::FeishuGroupPolicy;
using hermes::gateway::platforms::FeishuGroupRule;
using hermes::gateway::platforms::FeishuMessageType;
using hermes::gateway::platforms::FeishuRateLimiter;
using hermes::gateway::platforms::feishu_aes_decrypt;
using hermes::gateway::platforms::feishu_base64_decode;
using hermes::gateway::platforms::feishu_base64_encode;
using hermes::gateway::platforms::normalize_chat_type;
using hermes::gateway::platforms::normalize_feishu_message;
using hermes::gateway::platforms::normalize_feishu_text;
using hermes::gateway::platforms::parse_connection_mode;
using hermes::gateway::platforms::parse_feishu_post_content;
using hermes::gateway::platforms::parse_group_policy;
using hermes::gateway::platforms::parse_message_type;
using hermes::gateway::platforms::sha256_bytes;
using hermes::gateway::platforms::split_message_for_feishu;
using hermes::gateway::platforms::strip_markdown_to_plain_text;
using hermes::gateway::platforms::to_string;
using hermes::gateway::platforms::unique_lines;
using hermes::gateway::platforms::wrap_inline_code;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;

namespace {

HttpTransport::Response ok_json(const nlohmann::json& body,
                                int status = 200) {
    return {status, body.dump(), {}};
}

HttpTransport::Response token_ok(const std::string& tok = "t0") {
    return ok_json({{"code", 0},
                    {"msg", "ok"},
                    {"tenant_access_token", tok},
                    {"expire", 7200}});
}

FeishuAdapter::Config basic_cfg() {
    FeishuAdapter::Config c;
    c.app_id = "cli_a";
    c.app_secret = "sec_a";
    c.domain = "feishu";
    c.bot_open_id = "bot_open";
    c.bot_user_id = "bot_user";
    c.bot_name = "Hermes";
    return c;
}

}  // namespace

// ── Enum parsing ──────────────────────────────────────────────────────────

TEST(FeishuFull, ConnectionModeParse) {
    EXPECT_EQ(parse_connection_mode("websocket"), FeishuConnectionMode::WebSocket);
    EXPECT_EQ(parse_connection_mode("WS"), FeishuConnectionMode::WebSocket);
    EXPECT_EQ(parse_connection_mode("webhook"), FeishuConnectionMode::Webhook);
    EXPECT_EQ(parse_connection_mode("junk"), FeishuConnectionMode::Unknown);
    EXPECT_EQ(to_string(FeishuConnectionMode::Webhook), "webhook");
}

TEST(FeishuFull, GroupPolicyParse) {
    EXPECT_EQ(parse_group_policy("open"), FeishuGroupPolicy::Open);
    EXPECT_EQ(parse_group_policy("whitelist"), FeishuGroupPolicy::Allowlist);
    EXPECT_EQ(parse_group_policy("blacklist"), FeishuGroupPolicy::Blacklist);
    EXPECT_EQ(parse_group_policy("admin_only"), FeishuGroupPolicy::AdminOnly);
    EXPECT_EQ(parse_group_policy("disabled"), FeishuGroupPolicy::Disabled);
    EXPECT_EQ(to_string(FeishuGroupPolicy::Allowlist), "allowlist");
}

TEST(FeishuFull, MessageTypeParse) {
    EXPECT_EQ(parse_message_type("text"), FeishuMessageType::Text);
    EXPECT_EQ(parse_message_type("post"), FeishuMessageType::Post);
    EXPECT_EQ(parse_message_type("interactive"), FeishuMessageType::Interactive);
    EXPECT_EQ(parse_message_type("card"), FeishuMessageType::Interactive);
    EXPECT_EQ(parse_message_type("audio"), FeishuMessageType::Audio);
    EXPECT_EQ(parse_message_type("merge_forward"), FeishuMessageType::MergeForward);
    EXPECT_EQ(parse_message_type("???"), FeishuMessageType::Unknown);
}

// ── Text helpers ──────────────────────────────────────────────────────────

TEST(FeishuFull, EscapeMarkdown) {
    EXPECT_EQ(escape_markdown_text("a*b"), R"(a\*b)");
    EXPECT_EQ(escape_markdown_text("plain"), "plain");
    EXPECT_NE(escape_markdown_text("[link]"), "[link]");
}

TEST(FeishuFull, WrapInlineCode) {
    EXPECT_EQ(wrap_inline_code("hello"), "`hello`");
    // Contains a backtick → fence grows by one.
    EXPECT_EQ(wrap_inline_code("a`b"), "``a`b``");
    // Starts/ends with backtick → padded spaces.
    auto w = wrap_inline_code("`x");
    EXPECT_NE(w.find(" `x "), std::string::npos);
}

TEST(FeishuFull, NormalizeFeishuText) {
    EXPECT_EQ(normalize_feishu_text(""), "");
    EXPECT_EQ(normalize_feishu_text("  hello   world  "), "hello world");
    EXPECT_EQ(normalize_feishu_text("line1\r\nline2"), "line1\nline2");
    // @_user_42 → stripped space.
    auto s = normalize_feishu_text("hi @_user_42 there");
    EXPECT_EQ(s, "hi there");
}

TEST(FeishuFull, UniqueLines) {
    std::vector<std::string> in = {"a", "b", "a", "", "c", "b"};
    auto out = unique_lines(in);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "a");
    EXPECT_EQ(out[1], "b");
    EXPECT_EQ(out[2], "c");
}

TEST(FeishuFull, StripMarkdownToPlain) {
    auto s = strip_markdown_to_plain_text("**bold** and [link](http://x)");
    EXPECT_NE(s.find("bold"), std::string::npos);
    EXPECT_EQ(s.find("**"), std::string::npos);
    EXPECT_NE(s.find("(http://x)"), std::string::npos);
    auto f = strip_markdown_to_plain_text("```py\nprint(1)\n```");
    EXPECT_NE(f.find("print(1)"), std::string::npos);
    EXPECT_EQ(f.find("```"), std::string::npos);
}

TEST(FeishuFull, CoerceIntHelpers) {
    EXPECT_EQ(coerce_int(nlohmann::json(5)).value(), 5);
    EXPECT_EQ(coerce_int(nlohmann::json("42")).value(), 42);
    EXPECT_FALSE(coerce_int(nlohmann::json("abc")).has_value());
    EXPECT_FALSE(coerce_int(nlohmann::json(-1), /*min_value=*/0).has_value());
    EXPECT_EQ(coerce_required_int(nlohmann::json("bad"), 99), 99);
    EXPECT_EQ(coerce_required_int(nlohmann::json(7), 99), 7);
}

// ── Post payload round trips ──────────────────────────────────────────────

TEST(FeishuFull, BuildMarkdownPostPayloadShape) {
    auto raw = build_markdown_post_payload("**Hello**");
    auto parsed = nlohmann::json::parse(raw);
    ASSERT_TRUE(parsed.contains("zh_cn"));
    ASSERT_TRUE(parsed["zh_cn"]["content"].is_array());
    auto row = parsed["zh_cn"]["content"][0];
    EXPECT_EQ(row[0]["tag"], "md");
    EXPECT_EQ(row[0]["text"], "**Hello**");
}

TEST(FeishuFull, ParseFeishuPostContentBasic) {
    nlohmann::json p = {
        {"zh_cn", {
            {"title", "T"},
            {"content", nlohmann::json::array({
                nlohmann::json::array({
                    {{"tag", "text"}, {"text", "hello"}},
                    {{"tag", "at"},   {"user_id", "u1"},
                                       {"user_name", "alice"}},
                    {{"tag", "img"},  {"image_key", "k1"}, {"text", "pic"}},
                }),
                nlohmann::json::array({
                    {{"tag", "a"}, {"href", "http://x"}, {"text", "link"}},
                }),
            })}
        }}
    };
    auto res = parse_feishu_post_content(p.dump());
    EXPECT_NE(res.text_content.find("hello"), std::string::npos);
    EXPECT_NE(res.text_content.find("@alice"), std::string::npos);
    EXPECT_NE(res.text_content.find("[Image: pic]"), std::string::npos);
    EXPECT_NE(res.text_content.find("[link]"), std::string::npos);
    ASSERT_EQ(res.image_keys.size(), 1u);
    EXPECT_EQ(res.image_keys[0], "k1");
    ASSERT_EQ(res.mentioned_ids.size(), 1u);
    EXPECT_EQ(res.mentioned_ids[0], "u1");
}

TEST(FeishuFull, ParseFeishuPostContentBadJson) {
    auto res = parse_feishu_post_content("{not json");
    EXPECT_EQ(res.text_content, "[Rich text message]");
}

// ── normalize_feishu_message ──────────────────────────────────────────────

TEST(FeishuFull, NormalizeTextMessage) {
    auto n = normalize_feishu_message(
        "text", nlohmann::json{{"text", "hi   there"}}.dump());
    EXPECT_EQ(n.text_content, "hi there");
    EXPECT_EQ(n.raw_type, "text");
}

TEST(FeishuFull, NormalizeImageMessage) {
    auto n = normalize_feishu_message(
        "image", nlohmann::json{{"image_key", "kk"}, {"text", "label"}}.dump());
    ASSERT_EQ(n.image_keys.size(), 1u);
    EXPECT_EQ(n.image_keys[0], "kk");
    EXPECT_EQ(n.preferred_message_type, "photo");
    EXPECT_EQ(n.relation_kind, "image");
}

TEST(FeishuFull, NormalizeFileMessage) {
    auto n = normalize_feishu_message(
        "file", nlohmann::json{{"file_key", "fk"}, {"file_name", "doc.pdf"}}.dump());
    ASSERT_EQ(n.media_refs.size(), 1u);
    EXPECT_EQ(n.media_refs[0].file_key, "fk");
    EXPECT_EQ(n.relation_kind, "file");
    EXPECT_EQ(n.preferred_message_type, "document");
    EXPECT_EQ(n.metadata.value("placeholder_text", std::string()),
              "[Attachment: doc.pdf]");
}

TEST(FeishuFull, NormalizeInteractiveMessage) {
    nlohmann::json card = {
        {"header", {{"title", {{"content", "HDR"}, {"tag", "plain_text"}}}}},
        {"elements", nlohmann::json::array()}
    };
    auto n = normalize_feishu_message(
        "interactive", nlohmann::json{{"card", card}}.dump());
    EXPECT_EQ(n.relation_kind, "interactive");
    EXPECT_EQ(n.text_content, "HDR");
}

// ── Message segmentation ─────────────────────────────────────────────────

TEST(FeishuFull, SplitMessageShort) {
    auto v = split_message_for_feishu("hello world", 100);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "hello world");
}

TEST(FeishuFull, SplitMessageLongOnParagraphs) {
    std::string big(4000, 'a');
    std::string body = big + "\n\n" + std::string(4000, 'b');
    auto v = split_message_for_feishu(body, 4100);
    ASSERT_GE(v.size(), 2u);
    EXPECT_EQ(v[0].find("a"), 0u);
}

// ── Error classification ────────────────────────────────────────────────

TEST(FeishuFull, ClassifyErrors) {
    EXPECT_EQ(classify_feishu_error(500, nlohmann::json::object()).kind,
              FeishuErrorKind::Transient);
    EXPECT_EQ(classify_feishu_error(401, nlohmann::json::object()).kind,
              FeishuErrorKind::Unauthorized);
    EXPECT_EQ(classify_feishu_error(429, nlohmann::json::object()).kind,
              FeishuErrorKind::RateLimited);
    EXPECT_EQ(classify_feishu_error(
                  200, nlohmann::json{{"code", 99991663}}).kind,
              FeishuErrorKind::Unauthorized);
    EXPECT_EQ(classify_feishu_error(
                  200, nlohmann::json{{"code", 99991400}}).kind,
              FeishuErrorKind::RateLimited);
    EXPECT_EQ(classify_feishu_error(
                  200, nlohmann::json{{"code", 230011}}).kind,
              FeishuErrorKind::ReplyMissing);
    EXPECT_EQ(classify_feishu_error(
                  200, nlohmann::json{{"code", 0}}).kind,
              FeishuErrorKind::None);
}

TEST(FeishuFull, ChatTypeNormalization) {
    EXPECT_EQ(normalize_chat_type("P2P"), "private");
    EXPECT_EQ(normalize_chat_type("group"), "group");
    EXPECT_EQ(normalize_chat_type("channel"), "channel");
    EXPECT_EQ(normalize_chat_type("junk"), "unknown");
}

// ── Base64 + SHA256 + AES decrypt ───────────────────────────────────────

TEST(FeishuFull, Base64RoundTrip) {
    std::string src = "Hello, Feishu! 你好 🚀";
    auto enc = feishu_base64_encode(src);
    EXPECT_FALSE(enc.empty());
    auto dec = feishu_base64_decode(enc);
    EXPECT_EQ(dec, src);
}

TEST(FeishuFull, Base64EdgeCases) {
    EXPECT_EQ(feishu_base64_encode(""), "");
    EXPECT_EQ(feishu_base64_decode(""), "");
    // Padding scenarios.
    EXPECT_EQ(feishu_base64_decode("YQ=="), "a");
    EXPECT_EQ(feishu_base64_decode("YWI="), "ab");
    EXPECT_EQ(feishu_base64_decode("YWJj"), "abc");
}

TEST(FeishuFull, Sha256KnownVector) {
    auto digest = sha256_bytes("abc");
    ASSERT_EQ(digest.size(), 32u);
    // First byte of SHA256("abc") is 0xBA.
    EXPECT_EQ(static_cast<unsigned char>(digest[0]), 0xBA);
    EXPECT_EQ(static_cast<unsigned char>(digest[1]), 0x78);
}

TEST(FeishuFull, AesDecryptRejectsGarbage) {
    auto r = feishu_aes_decrypt("key", "!!!notb64!!!");
    EXPECT_FALSE(r.has_value());
}

// ── Rate limiter ────────────────────────────────────────────────────────

TEST(FeishuFull, RateLimiterAllowUntilCap) {
    FeishuRateLimiter rl;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(rl.allow("ip1", 5, 60.0, t0));
    }
    EXPECT_FALSE(rl.allow("ip1", 5, 60.0, t0));
    // After window rolls, allowed again.
    auto t1 = t0 + std::chrono::seconds(61);
    EXPECT_TRUE(rl.allow("ip1", 5, 60.0, t1));
}

TEST(FeishuFull, RateLimiterPerKey) {
    FeishuRateLimiter rl;
    auto t = std::chrono::steady_clock::now();
    EXPECT_TRUE(rl.allow("a", 1, 60.0, t));
    EXPECT_TRUE(rl.allow("b", 1, 60.0, t));
    EXPECT_FALSE(rl.allow("a", 1, 60.0, t));
    EXPECT_GE(rl.tracked_keys(), 2u);
}

// ── Anomaly tracker ────────────────────────────────────────────────────

TEST(FeishuFull, AnomalyThreshold) {
    FeishuAnomalyTracker tk(/*threshold=*/3);
    auto now = std::chrono::steady_clock::now();
    EXPECT_FALSE(tk.record("1.2.3.4", "500", now));
    EXPECT_FALSE(tk.record("1.2.3.4", "500", now));
    EXPECT_TRUE(tk.record("1.2.3.4", "500", now));
    // Further records do not re-alert.
    EXPECT_FALSE(tk.record("1.2.3.4", "500", now));
}

// ── Dedup cache ────────────────────────────────────────────────────────

TEST(FeishuFull, DedupCacheBasic) {
    FeishuDedupCache c(/*cap=*/4, /*ttl=*/60.0);
    auto now = std::chrono::steady_clock::now();
    EXPECT_TRUE(c.check_and_add("m1", now));
    EXPECT_FALSE(c.check_and_add("m1", now));
    EXPECT_TRUE(c.check_and_add("m2", now));
    // TTL expiry.
    auto later = now + std::chrono::seconds(120);
    EXPECT_TRUE(c.check_and_add("m1", later));
}

TEST(FeishuFull, DedupCacheEviction) {
    FeishuDedupCache c(/*cap=*/2, /*ttl=*/3600.0);
    auto now = std::chrono::steady_clock::now();
    c.check_and_add("a", now);
    c.check_and_add("b", now);
    c.check_and_add("c", now);
    EXPECT_LE(c.size(), 2u);
}

// ── Group policy gating ────────────────────────────────────────────────

TEST(FeishuFull, AllowGroupMessage_AdminAlwaysPasses) {
    auto cfg = basic_cfg();
    cfg.admins.insert("admin1");
    cfg.group_policy = "allowlist";
    FeishuAdapter a(cfg);
    EXPECT_TRUE(a.allow_group_message("admin1", "chat42"));
}

TEST(FeishuFull, AllowGroupMessage_AllowlistEnforced) {
    auto cfg = basic_cfg();
    cfg.group_policy = "allowlist";
    cfg.allowed_group_users.insert("user-ok");
    FeishuAdapter a(cfg);
    EXPECT_TRUE(a.allow_group_message("user-ok", "chatA"));
    EXPECT_FALSE(a.allow_group_message("user-bad", "chatA"));
}

TEST(FeishuFull, AllowGroupMessage_PerGroupRuleOverrides) {
    auto cfg = basic_cfg();
    cfg.group_policy = "allowlist";
    FeishuGroupRule rule;
    rule.policy = FeishuGroupPolicy::Open;
    cfg.group_rules["chatOpen"] = rule;
    FeishuAdapter a(cfg);
    EXPECT_TRUE(a.allow_group_message("anyone", "chatOpen"));
    EXPECT_FALSE(a.allow_group_message("anyone", "chatStrict"));
}

TEST(FeishuFull, AllowGroupMessage_Disabled) {
    auto cfg = basic_cfg();
    FeishuGroupRule rule;
    rule.policy = FeishuGroupPolicy::Disabled;
    cfg.group_rules["muted"] = rule;
    FeishuAdapter a(cfg);
    EXPECT_FALSE(a.allow_group_message("anyone", "muted"));
}

TEST(FeishuFull, MessageMentionsBot) {
    auto cfg = basic_cfg();
    FeishuAdapter a(cfg);
    nlohmann::json mentions = nlohmann::json::array({
        {{"id", {{"open_id", "someone"}}}, {"name", "Alice"}},
        {{"id", {{"open_id", "bot_open"}}}, {"name", "Hermes"}},
    });
    EXPECT_TRUE(a.message_mentions_bot(mentions));
    EXPECT_FALSE(a.message_mentions_bot(nlohmann::json::array()));
}

TEST(FeishuFull, ShouldAcceptGroupPrivateAlwaysAccepts) {
    auto cfg = basic_cfg();
    cfg.group_policy = "allowlist";
    FeishuAdapter a(cfg);
    nlohmann::json msg = {{"chat_type", "p2p"}};
    EXPECT_TRUE(a.should_accept_group_message(msg, "anyone", "dm"));
}

TEST(FeishuFull, ShouldAcceptGroupRequiresMention) {
    auto cfg = basic_cfg();
    cfg.group_policy = "allowlist";
    cfg.allowed_group_users.insert("user-ok");
    FeishuAdapter a(cfg);
    nlohmann::json msg = {{"chat_type", "group"},
                          {"mentions", nlohmann::json::array()}};
    EXPECT_FALSE(a.should_accept_group_message(msg, "user-ok", "chat-any"));
    msg["mentions"] = nlohmann::json::array({
        {{"id", {{"open_id", "bot_open"}}}, {"name", "Hermes"}},
    });
    EXPECT_TRUE(a.should_accept_group_message(msg, "user-ok", "chat-any"));
}

// ── Card builders ──────────────────────────────────────────────────────

TEST(FeishuFull, BuildCardMessageValidJson) {
    auto s = FeishuAdapter::build_card_message("Hello", "World");
    auto j = nlohmann::json::parse(s);
    EXPECT_EQ(j["msg_type"], "interactive");
    EXPECT_EQ(j["card"]["header"]["title"]["content"], "Hello");
}

TEST(FeishuFull, BuildApprovalCardHasFourButtons) {
    auto j = FeishuAdapter::build_approval_card("rm -rf /", "dangerous", 123);
    // elements[1] is the action row.
    auto actions = j["elements"][1]["actions"];
    EXPECT_EQ(actions.size(), 4u);
    EXPECT_EQ(actions[0]["value"]["hermes_action"], "approve_once");
    EXPECT_EQ(actions[0]["value"]["approval_id"], 123);
}

TEST(FeishuFull, BuildMenuCardItems) {
    auto j = FeishuAdapter::build_menu_card("Menu", {{"A", "act_a"}, {"B", "act_b"}});
    auto actions = j["elements"][0]["actions"];
    EXPECT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions[1]["text"]["content"], "B");
    EXPECT_EQ(actions[1]["value"]["hermes_action"], "act_b");
}

// ── Content builders ──────────────────────────────────────────────────

TEST(FeishuFull, ContentBuilders) {
    auto t = nlohmann::json::parse(FeishuAdapter::build_text_content("hi"));
    EXPECT_EQ(t["text"], "hi");
    auto i = nlohmann::json::parse(FeishuAdapter::build_image_content("ikey"));
    EXPECT_EQ(i["image_key"], "ikey");
    auto f = nlohmann::json::parse(FeishuAdapter::build_file_content("fk", "name.pdf"));
    EXPECT_EQ(f["file_key"], "fk");
    EXPECT_EQ(f["file_name"], "name.pdf");
}

TEST(FeishuFull, BuildOutboundPayloadDetectsMarkdown) {
    FeishuAdapter a(basic_cfg());
    auto plain = a.build_outbound_payload("hello world");
    EXPECT_EQ(plain.first, "text");
    auto md = a.build_outbound_payload("# Heading\nbody");
    EXPECT_EQ(md.first, "post");
}

// ── Token + URL composition ───────────────────────────────────────────

TEST(FeishuFull, UrlComposition) {
    FeishuAdapter a(basic_cfg());
    EXPECT_NE(a.auth_url().find("/open-apis/auth/v3/tenant_access_token/internal"),
              std::string::npos);
    EXPECT_NE(a.messages_url("chat_id").find("receive_id_type=chat_id"),
              std::string::npos);
    EXPECT_NE(a.reply_url("M1").find("/messages/M1/reply"), std::string::npos);
    EXPECT_NE(a.reaction_url("M1").find("/messages/M1/reactions"),
              std::string::npos);
    EXPECT_NE(a.chat_info_url("C").find("/chats/C"), std::string::npos);
    EXPECT_NE(a.image_upload_url().find("/im/v1/images"), std::string::npos);
}

TEST(FeishuFull, DomainSwitching) {
    auto cfg = basic_cfg();
    cfg.domain = "lark";
    FeishuAdapter a(cfg);
    EXPECT_NE(a.base_url().find("open.larksuite.com"), std::string::npos);
}

// ── Connect uses tenant_access_token flow ─────────────────────────────

TEST(FeishuFull, ConnectRefreshesToken) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("abc123"));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    EXPECT_EQ(a.tenant_access_token(), "abc123");
    EXPECT_FALSE(a.access_token_expired());
    ASSERT_EQ(t.requests().size(), 1u);
    auto body = nlohmann::json::parse(t.requests()[0].body);
    EXPECT_EQ(body["app_id"], "cli_a");
}

TEST(FeishuFull, ConnectRejectsBadCode) {
    FakeHttpTransport t;
    t.enqueue_response(ok_json({{"code", 10001}, {"msg", "bad secret"}}));
    FeishuAdapter a(basic_cfg(), &t);
    EXPECT_FALSE(a.connect());
}

TEST(FeishuFull, ConnectMissingCredsFails) {
    FeishuAdapter::Config cfg;  // empty
    FakeHttpTransport t;
    FeishuAdapter a(cfg, &t);
    EXPECT_FALSE(a.connect());
}

// ── send() posts text message with bearer ─────────────────────────────

TEST(FeishuFull, SendPostsTextWithBearer) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 0},
                                {"data", {{"message_id", "om_42"}}}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto res = a.send_message("oc_chat", "hello");
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.message_id, "om_42");
    ASSERT_EQ(t.requests().size(), 2u);
    const auto& send_req = t.requests()[1];
    auto it = send_req.headers.find("Authorization");
    ASSERT_NE(it, send_req.headers.end());
    EXPECT_NE(it->second.find("Bearer TKN"), std::string::npos);
    auto body = nlohmann::json::parse(send_req.body);
    EXPECT_EQ(body["receive_id"], "oc_chat");
    EXPECT_EQ(body["msg_type"], "text");
}

TEST(FeishuFull, SendPostFallsBackToPlainTextOnBadPost) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    // First send returns a post-content rejection.
    t.enqueue_response(ok_json(
        {{"code", 10002},
         {"msg", "content format of the post type is incorrect"}}));
    // Fallback to plain text succeeds.
    t.enqueue_response(
        ok_json({{"code", 0}, {"data", {{"message_id", "om_fb"}}}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto res = a.send_message("oc_chat", "# Heading");
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.message_id, "om_fb");
    // 3 requests total: auth + initial post + fallback text.
    EXPECT_EQ(t.requests().size(), 3u);
}

TEST(FeishuFull, SendCardRoundTrip) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 0},
                                {"data", {{"message_id", "om_card"}}}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto card = FeishuAdapter::build_approval_card("ls", "ok", 1);
    auto res = a.send_card("chat1", card);
    EXPECT_TRUE(res.ok);
    EXPECT_EQ(res.message_id, "om_card");
    auto body = nlohmann::json::parse(t.requests()[1].body);
    EXPECT_EQ(body["msg_type"], "interactive");
}

TEST(FeishuFull, ReplyToMessageFallbackOnReplyMissing) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 0},
                                {"data", {{"message_id", "om_ok"}}}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto r = a.reply_to_message("OM", "hi", false);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.message_id, "om_ok");
}

TEST(FeishuFull, EditMessageDispatches) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 0}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto r = a.edit_message("OM_1", "updated");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.message_id, "OM_1");
    EXPECT_NE(t.requests()[1].url.find("/messages/OM_1"), std::string::npos);
}

TEST(FeishuFull, AddReactionHitsReactionEndpoint) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 0}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    EXPECT_TRUE(a.add_reaction("OM_r", "OK"));
    EXPECT_NE(t.requests()[1].url.find("/messages/OM_r/reactions"),
              std::string::npos);
}

// ── Upload/download ──────────────────────────────────────────────────

TEST(FeishuFull, UploadImageExtractsKey) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(
        ok_json({{"code", 0}, {"data", {{"image_key", "IK_999"}}}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto k = a.upload_image("message", std::string("\x01\x02\x03", 3));
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(*k, "IK_999");
}

TEST(FeishuFull, UploadFileFailureReturnsEmpty) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({{"code", 1}, {"msg", "bad"}}));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto k = a.upload_file("stream", "a.txt", "data");
    EXPECT_FALSE(k.has_value());
}

TEST(FeishuFull, DownloadMessageResourceReturnsBytes) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    HttpTransport::Response file_resp;
    file_resp.status_code = 200;
    file_resp.body = std::string("\x89PNG\r\n", 6);
    file_resp.headers["Content-Disposition"] = "attachment; filename=\"x.png\"";
    t.enqueue_response(file_resp);
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    std::string name;
    auto bytes = a.download_message_resource("M", "K", "image", &name);
    EXPECT_EQ(bytes.size(), 6u);
    EXPECT_EQ(name, "x.png");
}

// ── Chat info + membership pagination ────────────────────────────────

TEST(FeishuFull, ListChatMembersPaginates) {
    FakeHttpTransport t;
    t.enqueue_response(token_ok("TKN"));
    t.enqueue_response(ok_json({
        {"code", 0},
        {"data", {
            {"items", nlohmann::json::array({
                {{"member_id", "u1"}},
                {{"member_id", "u2"}},
            })},
            {"has_more", true},
            {"page_token", "NEXT"},
        }},
    }));
    t.enqueue_response(ok_json({
        {"code", 0},
        {"data", {
            {"items", nlohmann::json::array({
                {{"member_id", "u3"}},
            })},
            {"has_more", false},
            {"page_token", ""},
        }},
    }));
    FeishuAdapter a(basic_cfg(), &t);
    ASSERT_TRUE(a.connect());
    auto members = a.list_chat_members("chat1");
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0]["member_id"], "u1");
    EXPECT_EQ(members[2]["member_id"], "u3");
    // Second page URL carries cursor.
    EXPECT_NE(t.requests().back().url.find("page_token=NEXT"),
              std::string::npos);
}

// ── Webhook verification ────────────────────────────────────────────

TEST(FeishuFull, WebhookPlainChallenge) {
    FeishuAdapter a(basic_cfg());
    nlohmann::json body = {{"type", "url_verification"},
                           {"challenge", "abc"}};
    nlohmann::json dec;
    auto resp = a.handle_webhook_body(body, &dec);
    EXPECT_EQ(resp.value("challenge", std::string()), "abc");
}

TEST(FeishuFull, WebhookDecryptsFailureNoKey) {
    FeishuAdapter a(basic_cfg());
    nlohmann::json body = {{"encrypt", "garbage"}};
    nlohmann::json dec;
    auto resp = a.handle_webhook_body(body, &dec);
    EXPECT_EQ(resp.value("code", -1), 400);
}

TEST(FeishuFull, WebhookPassesThroughPlainEvent) {
    FeishuAdapter a(basic_cfg());
    nlohmann::json body = {{"header",
                            {{"event_type", "im.message.receive_v1"}}},
                           {"event", {{"message", {{"chat_id", "C"}}}}}};
    nlohmann::json dec;
    auto resp = a.handle_webhook_body(body, &dec);
    EXPECT_EQ(resp.value("code", -1), 0);
    EXPECT_TRUE(dec.contains("event"));
}

TEST(FeishuFull, VerifyTokenChecks) {
    auto cfg = basic_cfg();
    cfg.verification_token = "VT123";
    FeishuAdapter a(cfg);
    EXPECT_TRUE(a.verify_token("VT123"));
    EXPECT_FALSE(a.verify_token("wrong"));
    EXPECT_FALSE(a.verify_token(""));
}

// ── Event classification and conversion ─────────────────────────────

TEST(FeishuFull, ClassifyEventKinds) {
    auto ev = [](const std::string& et) {
        return nlohmann::json{{"header", {{"event_type", et}}},
                              {"event", nlohmann::json::object()}};
    };
    EXPECT_EQ(FeishuAdapter::classify_event(ev("im.message.receive_v1")),
              "message");
    EXPECT_EQ(FeishuAdapter::classify_event(ev("im.message.reaction.created_v1")),
              "reaction");
    EXPECT_EQ(FeishuAdapter::classify_event(ev("card.action.trigger")),
              "card_action");
    EXPECT_EQ(FeishuAdapter::classify_event(
                  ev("im.chat.member.bot.added_v1")),
              "bot_added");
    EXPECT_EQ(FeishuAdapter::classify_event(nlohmann::json::object()),
              "unknown");
}

TEST(FeishuFull, EventToMessageTextP2P) {
    FeishuAdapter a(basic_cfg());
    nlohmann::json event = {
        {"header", {{"event_type", "im.message.receive_v1"}}},
        {"event", {
            {"sender", {{"sender_id", {{"open_id", "ou_sender"}}}}},
            {"message", {
                {"chat_id", "oc_chat"},
                {"chat_type", "p2p"},
                {"message_type", "text"},
                {"content", nlohmann::json({{"text", "  hi  "}}).dump()},
            }}
        }}
    };
    auto me = a.event_to_message(event);
    ASSERT_TRUE(me.has_value());
    EXPECT_EQ(me->text, "hi");
    EXPECT_EQ(me->source.chat_id, "oc_chat");
    EXPECT_EQ(me->source.user_id, "ou_sender");
    EXPECT_EQ(me->source.chat_type, "private");
}

TEST(FeishuFull, EventToMessageIgnoresNonMessage) {
    FeishuAdapter a(basic_cfg());
    nlohmann::json event = {
        {"header", {{"event_type", "card.action.trigger"}}}};
    EXPECT_FALSE(a.event_to_message(event).has_value());
}

// ── Approval register/take ───────────────────────────────────────────

TEST(FeishuFull, ApprovalRegisterAndTake) {
    FeishuAdapter a(basic_cfg());
    auto id1 = a.register_approval("sess1", "chatA", "om1");
    auto id2 = a.register_approval("sess2", "chatB", "om2");
    EXPECT_NE(id1, id2);
    auto t = a.take_approval(id1);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->session_key, "sess1");
    EXPECT_EQ(t->chat_id, "chatA");
    // Second take for same ID fails.
    EXPECT_FALSE(a.take_approval(id1).has_value());
    // id2 still there.
    EXPECT_TRUE(a.take_approval(id2).has_value());
}

// ── last_error_kind mapping ─────────────────────────────────────────

TEST(FeishuFull, LastErrorKindFromAuthFailure) {
    FakeHttpTransport t;
    t.enqueue_response(ok_json({{"code", 99991663}, {"msg", "token bad"}},
                               /*status=*/200));
    FeishuAdapter a(basic_cfg(), &t);
    EXPECT_FALSE(a.connect());
    EXPECT_EQ(a.last_error_kind(),
              hermes::gateway::AdapterErrorKind::Fatal);
}

TEST(FeishuFull, SendFailsWithoutTransport) {
    FeishuAdapter a(basic_cfg(), nullptr);
    // No transport (and no default curl): send should gracefully fail.
    auto r = a.send_message("X", "body");
    EXPECT_FALSE(r.ok);
}
