// Tests for the depth-port helpers added across api_server, bluebubbles,
// mattermost, webhook, and home_assistant platform adapters.
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../platforms/api_server.hpp"
#include "../platforms/bluebubbles.hpp"
#include "../platforms/home_assistant.hpp"
#include "../platforms/mattermost.hpp"
#include "../platforms/webhook.hpp"

using namespace hermes::gateway::platforms;

// ---------------------------------------------------------------------------
// BlueBubbles helpers
// ---------------------------------------------------------------------------

TEST(BlueBubblesHelpers, NormalizeServerUrl) {
    EXPECT_EQ(bb_normalize_server_url("example.com"), "http://example.com");
    EXPECT_EQ(bb_normalize_server_url("https://Example.com/"),
              "https://Example.com");
    EXPECT_EQ(bb_normalize_server_url("  http://x:8080/// "), "http://x:8080");
    EXPECT_EQ(bb_normalize_server_url(""), "");
}

TEST(BlueBubblesHelpers, RedactPii) {
    auto out = bb_redact("call +14155551212 or me@example.com");
    EXPECT_NE(out.find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(out.find("+14155551212"), std::string::npos);
    EXPECT_EQ(out.find("me@example.com"), std::string::npos);
}

TEST(BlueBubblesHelpers, StripMarkdown) {
    EXPECT_EQ(bb_strip_markdown("**bold** and *italic*"), "bold and italic");
    EXPECT_EQ(bb_strip_markdown("`code` and __more__"), "code and more");
    EXPECT_EQ(bb_strip_markdown("# Title\n\ncontent"), "Title\n\ncontent");
    EXPECT_EQ(bb_strip_markdown("[link](http://x)"), "link");
}

TEST(BlueBubblesHelpers, UrlQuote) {
    EXPECT_EQ(bb_url_quote("hello world"), "hello%20world");
    EXPECT_EQ(bb_url_quote("a/b?c"), "a%2Fb%3Fc");
    EXPECT_EQ(bb_url_quote("safe-_.~"), "safe-_.~");
}

TEST(BlueBubblesHelpers, LooksLikeHandle) {
    EXPECT_TRUE(bb_looks_like_handle("user@example.com"));
    EXPECT_TRUE(bb_looks_like_handle("+14155551212"));
    EXPECT_FALSE(bb_looks_like_handle("iMessage;-;chatid"));
    EXPECT_FALSE(bb_looks_like_handle(""));
}

TEST(BlueBubblesHelpers, IsGroupChat) {
    EXPECT_TRUE(bb_is_group_chat("iMessage;+;abc123"));
    EXPECT_FALSE(bb_is_group_chat("iMessage;-;single"));
}

TEST(BlueBubblesHelpers, NormalizeReaction) {
    EXPECT_EQ(bb_normalize_reaction("LOVE"), "love");
    EXPECT_EQ(bb_normalize_reaction("heart"), "love");
    EXPECT_EQ(bb_normalize_reaction("thumbs_up"), "like");
    EXPECT_EQ(bb_normalize_reaction("haha"), "laugh");
    EXPECT_EQ(bb_normalize_reaction("???"), "");
}

TEST(BlueBubblesHelpers, ClassifyEventType) {
    EXPECT_EQ(bb_classify_event_type({{"type", "new-message"}}), "message");
    EXPECT_EQ(bb_classify_event_type({{"event", "typing-indicator"}}), "typing");
    EXPECT_EQ(bb_classify_event_type({{"type", "read-status-update"}}), "read");
    EXPECT_EQ(bb_classify_event_type({{"type", "unknown-thing"}}), "unknown");
}

TEST(BlueBubblesHelpers, ExtractAttachments) {
    nlohmann::json msg = {{"attachments", nlohmann::json::array(
                                              {{{"guid", "g1"},
                                                {"mimeType", "image/png"},
                                                {"transferName", "x.png"}},
                                               {{"guid", "g2"},
                                                {"mimeType", "audio/mp3"}}})}};
    auto out = bb_extract_attachments(msg);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0]["guid"], "g1");
    EXPECT_EQ(out[0]["mime_type"], "image/png");
    EXPECT_EQ(out[1]["mime_type"], "audio/mp3");
}

TEST(BlueBubblesAdapter, ApiUrlBuildsPasswordSeparator) {
    BlueBubblesAdapter::Config cfg;
    cfg.server_url = "http://localhost:8765";
    cfg.password = "p ass";  // space → must be encoded
    BlueBubblesAdapter a(cfg);
    EXPECT_EQ(a.api_url("/api/v1/ping"),
              "http://localhost:8765/api/v1/ping?password=p%20ass");
    EXPECT_EQ(a.api_url("/api/v1/x?foo=1"),
              "http://localhost:8765/api/v1/x?foo=1&password=p%20ass");
}

TEST(BlueBubblesAdapter, WebhookExternalUrlLocalRemap) {
    BlueBubblesAdapter::Config cfg;
    cfg.server_url = "http://localhost:8765";
    cfg.password = "x";
    cfg.webhook_host = "0.0.0.0";
    cfg.webhook_port = 9999;
    cfg.webhook_path = "/hook";
    BlueBubblesAdapter a(cfg);
    EXPECT_EQ(a.webhook_external_url(), "http://localhost:9999/hook");
}

TEST(BlueBubblesAdapter, BuildTextPayloadIncludesReply) {
    BlueBubblesAdapter::Config cfg;
    cfg.server_url = "http://x";
    cfg.password = "y";
    BlueBubblesAdapter a(cfg);
    auto p1 = a.build_text_payload("guid", "hi", std::nullopt);
    EXPECT_EQ(p1["chatGuid"], "guid");
    EXPECT_FALSE(p1.contains("method"));
}

TEST(BlueBubblesAdapter, GuidCacheManagement) {
    BlueBubblesAdapter::Config cfg;
    cfg.server_url = "http://x";
    cfg.password = "y";
    BlueBubblesAdapter a(cfg);
    EXPECT_EQ(a.guid_cache_size(), 0u);
    a.clear_guid_cache();
    EXPECT_EQ(a.guid_cache_size(), 0u);
}

// ---------------------------------------------------------------------------
// API Server helpers
// ---------------------------------------------------------------------------

TEST(ApiServerHelpers, OpenAIErrorEnvelope) {
    auto err = api_openai_error("bad", "invalid_request_error", "field", "code1");
    ASSERT_TRUE(err.contains("error"));
    EXPECT_EQ(err["error"]["message"], "bad");
    EXPECT_EQ(err["error"]["type"], "invalid_request_error");
    EXPECT_EQ(err["error"]["param"], "field");
    EXPECT_EQ(err["error"]["code"], "code1");
}

TEST(ApiServerHelpers, DeriveSessionIdDeterministic) {
    auto a = api_derive_chat_session_id("sys", "first user msg");
    auto b = api_derive_chat_session_id("sys", "first user msg");
    auto c = api_derive_chat_session_id("sys", "different");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.substr(0, 4), "api-");
    EXPECT_EQ(a.size(), 20u);  // "api-" + 16 hex
}

TEST(ApiServerHelpers, JobIdValidation) {
    EXPECT_TRUE(api_is_valid_job_id("0123456789ab"));
    EXPECT_TRUE(api_is_valid_job_id("ffeeddccbbaa"));
    EXPECT_FALSE(api_is_valid_job_id("ABC123456789"));  // uppercase
    EXPECT_FALSE(api_is_valid_job_id("0123456"));       // too short
    EXPECT_FALSE(api_is_valid_job_id("0123456789abcd"));  // too long
    EXPECT_FALSE(api_is_valid_job_id("0123456789xx"));  // non-hex
}

TEST(ApiServerHelpers, ParseCorsOrigins) {
    auto a = api_parse_cors_origins("a.com, b.com,*");
    ASSERT_EQ(a.size(), 3u);
    EXPECT_EQ(a[0], "a.com");
    EXPECT_EQ(a[1], "b.com");
    EXPECT_EQ(a[2], "*");
    EXPECT_TRUE(api_parse_cors_origins("").empty());
}

TEST(ApiServerHelpers, CorsHeaderResolution) {
    std::vector<std::string> wildcard{"*"};
    auto h = api_cors_headers_for_origin(wildcard, "https://anything");
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->at("Access-Control-Allow-Origin"), "*");

    std::vector<std::string> allowed{"https://app.example.com"};
    EXPECT_FALSE(api_cors_headers_for_origin(allowed, "https://other.com").has_value());
    auto ok = api_cors_headers_for_origin(allowed, "https://app.example.com");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->at("Access-Control-Allow-Origin"), "https://app.example.com");
    EXPECT_EQ(ok->at("Vary"), "Origin");
}

TEST(ApiServerHelpers, OriginAllowedNonBrowser) {
    EXPECT_TRUE(api_origin_allowed({}, ""));         // empty origin
    EXPECT_FALSE(api_origin_allowed({}, "https://x"));
    EXPECT_TRUE(api_origin_allowed({"*"}, "https://anything"));
}

TEST(ApiServerHelpers, ResolveModelName) {
    EXPECT_EQ(api_resolve_model_name("override", "myprofile"), "override");
    EXPECT_EQ(api_resolve_model_name("", "myprofile"), "myprofile");
    EXPECT_EQ(api_resolve_model_name("", "default"), "hermes-agent");
    EXPECT_EQ(api_resolve_model_name("", ""), "hermes-agent");
}

TEST(ApiServerHelpers, BodySizeCheck) {
    EXPECT_EQ(api_check_body_size(""), 0);
    EXPECT_EQ(api_check_body_size("100"), 0);
    EXPECT_EQ(api_check_body_size("9999999"), 413);
    EXPECT_EQ(api_check_body_size("not-a-number"), 400);
}

TEST(ApiServerHelpers, ParseChatCompletionMessages) {
    nlohmann::json msgs = nlohmann::json::array(
        {{{"role", "system"}, {"content", "you are X"}},
         {{"role", "user"}, {"content", "hi"}},
         {{"role", "assistant"}, {"content", "hello"}},
         {{"role", "user"}, {"content", "again"}}});
    auto p = api_parse_chat_completion_messages(msgs);
    EXPECT_EQ(p.error, "");
    EXPECT_EQ(p.system_prompt, "you are X");
    EXPECT_EQ(p.user_message, "again");
    EXPECT_EQ(p.history.size(), 2u);
    EXPECT_EQ(p.history[0]["role"], "user");
    EXPECT_EQ(p.history[1]["role"], "assistant");
}

TEST(ApiServerHelpers, ParseChatCompletionRejectsEmpty) {
    auto p = api_parse_chat_completion_messages(nlohmann::json::object());
    EXPECT_NE(p.error, "");
}

TEST(ApiServerHelpers, BuildChatCompletionResponse) {
    auto r = api_build_chat_completion_response("c1", "m1", 100, "hi", 5, 7);
    EXPECT_EQ(r["id"], "c1");
    EXPECT_EQ(r["model"], "m1");
    EXPECT_EQ(r["choices"][0]["message"]["content"], "hi");
    EXPECT_EQ(r["usage"]["total_tokens"], 12);
}

TEST(ApiServerHelpers, BearerAuth) {
    EXPECT_TRUE(api_check_bearer_auth("", "anything"));   // no key configured
    EXPECT_TRUE(api_check_bearer_auth("secret", "Bearer secret"));
    EXPECT_FALSE(api_check_bearer_auth("secret", "Bearer wrong"));
    EXPECT_FALSE(api_check_bearer_auth("secret", "Basic secret"));
    EXPECT_FALSE(api_check_bearer_auth("secret", ""));
}

TEST(ApiServerHelpers, RequestFingerprintStable) {
    nlohmann::json a = {{"x", 1}, {"y", "z"}};
    nlohmann::json b = {{"y", "z"}, {"x", 1}};
    auto fa = api_make_request_fingerprint(a, {"x", "y"});
    auto fb = api_make_request_fingerprint(b, {"x", "y"});
    EXPECT_EQ(fa, fb);
}

// --- ResponseStore & IdempotencyCache --------------------------------------

TEST(ResponseStoreTest, PutGetEvict) {
    ResponseStore store(2);
    store.put("a", {{"x", 1}});
    store.put("b", {{"x", 2}});
    EXPECT_TRUE(store.get("a").has_value());
    store.put("c", {{"x", 3}});  // evicts b (LRU; a was just touched)
    EXPECT_TRUE(store.get("a").has_value());
    EXPECT_FALSE(store.get("b").has_value());
    EXPECT_TRUE(store.get("c").has_value());
    EXPECT_EQ(store.size(), 2u);
}

TEST(ResponseStoreTest, ConversationMap) {
    ResponseStore store;
    EXPECT_FALSE(store.get_conversation("conv1").has_value());
    store.set_conversation("conv1", "resp_42");
    auto v = store.get_conversation("conv1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "resp_42");
}

TEST(IdempotencyCacheTest, PutGetMatchesFingerprint) {
    IdempotencyCache cache(10, std::chrono::seconds(60));
    cache.put("k", "fp1", {{"r", 1}});
    EXPECT_TRUE(cache.get("k", "fp1").has_value());
    EXPECT_FALSE(cache.get("k", "fp2").has_value());  // wrong fingerprint
    EXPECT_FALSE(cache.get("missing", "fp1").has_value());
}

TEST(ApiServerAdapter, HmacSignatureRoundTrip) {
    auto body = std::string{"hello world"};
    // Compute via webhook helper (uses same OpenSSL HMAC primitive).
    auto sig = webhook_compute_hmac_sha256("secret", body);
    EXPECT_TRUE(ApiServerAdapter::verify_hmac_signature("secret", body, sig));
    EXPECT_FALSE(ApiServerAdapter::verify_hmac_signature("wrong", body, sig));
}

TEST(ApiServerAdapter, PendingResponseQueue) {
    ApiServerAdapter::Config cfg;
    ApiServerAdapter a(cfg);
    EXPECT_TRUE(a.send("chat1", "hello"));
    EXPECT_EQ(a.get_pending_response("chat1"), "hello");
    EXPECT_EQ(a.get_pending_response("chat1"), "");  // consumed
}

// ---------------------------------------------------------------------------
// Mattermost helpers
// ---------------------------------------------------------------------------

TEST(MattermostHelpers, ChannelTypeMap) {
    EXPECT_EQ(mm_channel_type_to_chat_type("D"), "dm");
    EXPECT_EQ(mm_channel_type_to_chat_type("G"), "group");
    EXPECT_EQ(mm_channel_type_to_chat_type("P"), "group");
    EXPECT_EQ(mm_channel_type_to_chat_type("O"), "channel");
    EXPECT_EQ(mm_channel_type_to_chat_type("?"), "channel");
}

TEST(MattermostHelpers, WebsocketUrl) {
    EXPECT_EQ(mm_websocket_url("https://mm.example.com"),
              "wss://mm.example.com/api/v4/websocket");
    EXPECT_EQ(mm_websocket_url("http://mm/"), "ws://mm/api/v4/websocket");
}

TEST(MattermostHelpers, StripImageMarkdown) {
    EXPECT_EQ(mm_strip_image_markdown("hi ![alt](http://img/x.png) bye"),
              "hi http://img/x.png bye");
    EXPECT_EQ(mm_strip_image_markdown("nothing"), "nothing");
}

TEST(MattermostHelpers, AuthChallenge) {
    auto j = mm_build_auth_challenge("tok", 42);
    EXPECT_EQ(j["seq"], 42);
    EXPECT_EQ(j["action"], "authentication_challenge");
    EXPECT_EQ(j["data"]["token"], "tok");
}

TEST(MattermostHelpers, PostPayloadWithThread) {
    auto p1 = mm_build_post_payload("ch", "hi");
    EXPECT_FALSE(p1.contains("root_id"));
    auto p2 = mm_build_post_payload("ch", "hi", "root1");
    EXPECT_EQ(p2["root_id"], "root1");
}

TEST(MattermostHelpers, MentionGating) {
    EXPECT_TRUE(mm_message_mentions_bot("hello @bot please", "uid", "bot"));
    EXPECT_TRUE(mm_message_mentions_bot("ping @uid", "uid", "bot"));
    EXPECT_FALSE(mm_message_mentions_bot("just chatting", "uid", "bot"));
}

TEST(MattermostHelpers, StripMentions) {
    EXPECT_EQ(mm_strip_mentions("@bot do this", "u", "bot"), "do this");
    EXPECT_EQ(mm_strip_mentions("hi @BOT and @u thanks", "u", "bot"),
              "hi  and  thanks");
}

TEST(MattermostHelpers, ParsePostedEvent) {
    nlohmann::json post_inner = {{"id", "p1"}, {"message", "hi"},
                                  {"user_id", "u1"}};
    nlohmann::json ev = {
        {"event", "posted"},
        {"data", {{"channel_type", "D"}, {"sender_name", "@alice"},
                  {"post", post_inner.dump()}}}};
    auto parsed = mm_parse_posted_event(ev);
    EXPECT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.event_type, "posted");
    EXPECT_EQ(parsed.channel_type_raw, "D");
    EXPECT_EQ(parsed.sender_name, "alice");
    EXPECT_EQ(parsed.post["id"], "p1");
}

TEST(MattermostHelpers, ClassifyMessageType) {
    EXPECT_EQ(mm_classify_message_type("hi", {}), "TEXT");
    EXPECT_EQ(mm_classify_message_type("/help", {}), "COMMAND");
    EXPECT_EQ(mm_classify_message_type("", {"image/png"}), "PHOTO");
    EXPECT_EQ(mm_classify_message_type("", {"audio/ogg"}), "VOICE");
    EXPECT_EQ(mm_classify_message_type("", {"application/pdf"}), "DOCUMENT");
}

TEST(MattermostAdapter, DedupSeenAndPrune) {
    MattermostAdapter::Config cfg;
    cfg.token = "t";
    cfg.url = "http://mm";
    MattermostAdapter a(cfg);
    EXPECT_FALSE(a.seen_post("p1"));
    a.mark_seen("p1");
    EXPECT_TRUE(a.seen_post("p1"));
    EXPECT_EQ(a.seen_size(), 1u);
}

TEST(MattermostAdapter, BotIdentitySetter) {
    MattermostAdapter::Config cfg;
    cfg.token = "t";
    cfg.url = "http://mm";
    MattermostAdapter a(cfg);
    a.set_bot_identity("uid", "alice");
    EXPECT_EQ(a.bot_user_id(), "uid");
    EXPECT_EQ(a.bot_username(), "alice");
}

// ---------------------------------------------------------------------------
// Webhook helpers
// ---------------------------------------------------------------------------

TEST(WebhookHelpers, HmacSha256Hex) {
    auto sig = webhook_compute_hmac_sha256("key", "data");
    EXPECT_EQ(sig.size(), 64u);
    for (char c : sig) EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
}

TEST(WebhookHelpers, ValidateGitHubSignature) {
    std::string body = "{\"x\":1}";
    auto expected = "sha256=" + webhook_compute_hmac_sha256("secret", body);
    std::unordered_map<std::string, std::string> headers{
        {"X-Hub-Signature-256", expected}};
    EXPECT_TRUE(webhook_validate_signature(headers, body, "secret"));
    EXPECT_FALSE(webhook_validate_signature(headers, body, "wrong"));
}

TEST(WebhookHelpers, ValidateGitLabToken) {
    std::unordered_map<std::string, std::string> headers{
        {"X-Gitlab-Token", "secret"}};
    EXPECT_TRUE(webhook_validate_signature(headers, "body", "secret"));
    EXPECT_FALSE(webhook_validate_signature(headers, "body", "other"));
}

TEST(WebhookHelpers, ValidateGenericSignature) {
    std::string body = "payload";
    auto expected = webhook_compute_hmac_sha256("secret", body);
    std::unordered_map<std::string, std::string> headers{
        {"X-Webhook-Signature", expected}};
    EXPECT_TRUE(webhook_validate_signature(headers, body, "secret"));
}

TEST(WebhookHelpers, ExtractEventType) {
    std::unordered_map<std::string, std::string> gh{
        {"X-GitHub-Event", "pull_request"}};
    EXPECT_EQ(webhook_extract_event_type(gh, {}), "pull_request");
    std::unordered_map<std::string, std::string> empty;
    nlohmann::json payload = {{"event_type", "deploy"}};
    EXPECT_EQ(webhook_extract_event_type(empty, payload), "deploy");
    EXPECT_EQ(webhook_extract_event_type(empty, {}), "unknown");
}

TEST(WebhookHelpers, RenderPromptDotNotation) {
    nlohmann::json payload = {
        {"pull_request", {{"title", "Add feature X"}, {"number", 42}}}};
    auto out = webhook_render_prompt(
        "PR #{pull_request.number}: {pull_request.title}", payload, "pr",
        "route");
    EXPECT_EQ(out, "PR #42: Add feature X");
}

TEST(WebhookHelpers, RenderPromptMissingKeyPreserved) {
    nlohmann::json payload = {{"x", 1}};
    auto out =
        webhook_render_prompt("got {missing.key}", payload, "e", "r");
    EXPECT_EQ(out, "got {missing.key}");
}

TEST(WebhookHelpers, RenderPromptRawDump) {
    nlohmann::json payload = {{"x", 1}};
    auto out = webhook_render_prompt("payload: {__raw__}", payload, "", "");
    EXPECT_NE(out.find("\"x\": 1"), std::string::npos);
}

TEST(WebhookHelpers, BuildSessionChatId) {
    EXPECT_EQ(webhook_build_session_chat_id("github", "abc"),
              "webhook:github:abc");
}

TEST(WebhookHelpers, ExtractDeliveryIdHeaderPrecedence) {
    std::unordered_map<std::string, std::string> h{
        {"X-GitHub-Delivery", "gh-1"}, {"X-Request-ID", "req-2"}};
    EXPECT_EQ(webhook_extract_delivery_id(h, std::chrono::system_clock::now()),
              "gh-1");
}

TEST(WebhookHelpers, ValidateRoutesRequiresSecret) {
    std::unordered_map<std::string, WebhookRoute> routes;
    routes["a"] = {};
    routes["a"].name = "a";
    auto errs = webhook_validate_routes(routes, "");
    EXPECT_EQ(errs.size(), 1u);
    auto ok = webhook_validate_routes(routes, "global");
    EXPECT_TRUE(ok.empty());
}

TEST(WebhookAdapter, RateLimitWindow) {
    WebhookAdapter::Config cfg;
    cfg.rate_limit_per_minute = 2;
    WebhookAdapter a(cfg);
    auto now = std::chrono::system_clock::now();
    EXPECT_TRUE(a.record_rate_limit("r", now));
    EXPECT_TRUE(a.record_rate_limit("r", now));
    EXPECT_FALSE(a.record_rate_limit("r", now));  // exceeded
    EXPECT_EQ(a.rate_window_size("r"), 2u);
}

TEST(WebhookAdapter, IdempotencyDedup) {
    WebhookAdapter::Config cfg;
    WebhookAdapter a(cfg);
    auto now = std::chrono::system_clock::now();
    EXPECT_FALSE(a.seen_delivery("d1"));
    a.mark_delivery_seen("d1", now);
    EXPECT_TRUE(a.seen_delivery("d1"));
    EXPECT_EQ(a.seen_size(), 1u);
}

TEST(WebhookAdapter, DeliveryInfoStore) {
    WebhookAdapter::Config cfg;
    WebhookAdapter a(cfg);
    auto now = std::chrono::system_clock::now();
    a.store_delivery_info("webhook:r:1", {{"deliver", "log"}}, now);
    auto info = a.get_delivery_info("webhook:r:1");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ((*info)["deliver"], "log");
}

TEST(WebhookAdapter, BodySizeCheck) {
    WebhookAdapter::Config cfg;
    cfg.max_body_bytes = 100;
    WebhookAdapter a(cfg);
    EXPECT_EQ(a.check_body_size(50), 0);
    EXPECT_EQ(a.check_body_size(200), 413);
}

// ---------------------------------------------------------------------------
// Home Assistant helpers
// ---------------------------------------------------------------------------

TEST(HaHelpers, WebsocketUrl) {
    EXPECT_EQ(ha_websocket_url("https://ha.local:8123"),
              "wss://ha.local:8123/api/websocket");
    EXPECT_EQ(ha_websocket_url("http://h/"), "ws://h/api/websocket");
}

TEST(HaHelpers, AuthAndSubscribePayloads) {
    auto a = ha_build_auth_payload("xtok");
    EXPECT_EQ(a["type"], "auth");
    EXPECT_EQ(a["access_token"], "xtok");
    auto s = ha_build_subscribe_events(7);
    EXPECT_EQ(s["id"], 7);
    EXPECT_EQ(s["type"], "subscribe_events");
    EXPECT_EQ(s["event_type"], "state_changed");
}

TEST(HaHelpers, ExtractDomain) {
    EXPECT_EQ(ha_extract_domain("light.kitchen"), "light");
    EXPECT_EQ(ha_extract_domain("noseparator"), "");
}

TEST(HaHelpers, ShouldSkipEntity) {
    std::unordered_set<std::string> dom{"light"};
    std::unordered_set<std::string> ent;
    std::unordered_set<std::string> ign{"light.private"};
    EXPECT_TRUE(ha_should_skip_entity("", dom, ent, ign, false));
    EXPECT_TRUE(ha_should_skip_entity("light.private", dom, ent, ign, false));
    EXPECT_FALSE(ha_should_skip_entity("light.kitchen", dom, ent, ign, false));
    EXPECT_TRUE(ha_should_skip_entity("switch.kitchen", dom, ent, ign, false));
    EXPECT_FALSE(ha_should_skip_entity("switch.kitchen", {}, {}, {}, true));
    EXPECT_TRUE(ha_should_skip_entity("switch.kitchen", {}, {}, {}, false));
}

TEST(HaHelpers, FormatStateChangeLight) {
    nlohmann::json old_s = {{"state", "off"}};
    nlohmann::json new_s = {{"state", "on"},
                             {"attributes", {{"friendly_name", "Hall"}}}};
    auto out = ha_format_state_change("light.hall", old_s, new_s);
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("Hall"), std::string::npos);
    EXPECT_NE(out->find("turned on"), std::string::npos);
}

TEST(HaHelpers, FormatStateChangeUnchangedDropped) {
    nlohmann::json old_s = {{"state", "on"}};
    nlohmann::json new_s = {{"state", "on"}};
    EXPECT_FALSE(ha_format_state_change("light.x", old_s, new_s).has_value());
}

TEST(HaHelpers, FormatStateChangeBinarySensor) {
    auto out = ha_format_state_change(
        "binary_sensor.door", {{"state", "off"}},
        {{"state", "on"},
         {"attributes", {{"friendly_name", "Front Door"}}}});
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("triggered"), std::string::npos);
}

TEST(HaHelpers, FormatStateChangeSensorWithUnit) {
    auto out = ha_format_state_change(
        "sensor.temp", {{"state", "20"}},
        {{"state", "21"},
         {"attributes", {{"friendly_name", "Temp"}, {"unit_of_measurement", "C"}}}});
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("20C"), std::string::npos);
    EXPECT_NE(out->find("21C"), std::string::npos);
}

TEST(HaAdapter, CooldownGuard) {
    HomeAssistantAdapter::Config cfg;
    cfg.hass_token = "t";
    cfg.cooldown = std::chrono::seconds(30);
    HomeAssistantAdapter a(cfg);
    auto now = std::chrono::system_clock::now();
    EXPECT_TRUE(a.record_event("light.x", now));
    EXPECT_FALSE(a.record_event("light.x", now + std::chrono::seconds(1)));
    EXPECT_TRUE(a.record_event("light.x", now + std::chrono::seconds(31)));
    EXPECT_EQ(a.cooldown_size(), 1u);
}

TEST(HaAdapter, NotificationPayloadTruncates) {
    HomeAssistantAdapter::Config cfg;
    cfg.hass_token = "t";
    HomeAssistantAdapter a(cfg);
    std::string big(kHaMaxMessageLength + 100, 'X');
    auto p = a.build_notification_payload(big);
    EXPECT_EQ(p["title"], "Hermes Agent");
    EXPECT_EQ(p["message"].get<std::string>().size(), kHaMaxMessageLength);
}

TEST(HaAdapter, MessageIdMonotonic) {
    HomeAssistantAdapter::Config cfg;
    cfg.hass_token = "t";
    HomeAssistantAdapter a(cfg);
    EXPECT_EQ(a.next_message_id(), 1);
    EXPECT_EQ(a.next_message_id(), 2);
    EXPECT_EQ(a.next_message_id(), 3);
}
