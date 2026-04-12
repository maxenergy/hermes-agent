// Phase 12 — Platform adapter tests.
#include <gtest/gtest.h>

#include "../platforms/adapter_factory.hpp"
#include "../platforms/api_server.hpp"
#include "../platforms/discord.hpp"
#include "../platforms/email.hpp"
#include "../platforms/local.hpp"
#include "../platforms/slack.hpp"
#include "../platforms/telegram.hpp"
#include "../platforms/webhook.hpp"
#include "../platforms/weixin.hpp"

#include <hermes/llm/llm_client.hpp>
#include <sstream>

using namespace hermes::gateway;
using namespace hermes::gateway::platforms;
using namespace hermes::llm;

// --- Telegram ---

TEST(TelegramAdapter, ConnectWithTokenSucceeds) {
    FakeHttpTransport fake;
    // Enqueue a getMe response.
    fake.enqueue_response({200, R"({"ok":true,"result":{"id":123,"username":"testbot"}})", {}});

    TelegramAdapter::Config cfg;
    cfg.bot_token = "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11";
    TelegramAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.connect());
    EXPECT_TRUE(adapter.connected());
}

TEST(TelegramAdapter, ConnectEmptyTokenFails) {
    TelegramAdapter::Config cfg;
    TelegramAdapter adapter(cfg);
    EXPECT_FALSE(adapter.connect());
}

TEST(TelegramAdapter, ConnectBadResponseFails) {
    FakeHttpTransport fake;
    fake.enqueue_response({401, R"({"ok":false})", {}});

    TelegramAdapter::Config cfg;
    cfg.bot_token = "bad-token";
    TelegramAdapter adapter(cfg, &fake);
    EXPECT_FALSE(adapter.connect());
}

TEST(TelegramAdapter, PlatformReturnsTelegram) {
    TelegramAdapter::Config cfg;
    TelegramAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Telegram);
}

TEST(TelegramAdapter, FormatMarkdownV2Escapes) {
    auto result = TelegramAdapter::format_markdown_v2("hello_world*bold*");
    EXPECT_EQ(result, "hello\\_world\\*bold\\*");
}

TEST(TelegramAdapter, FormatMarkdownV2EscapesAllSpecials) {
    auto result = TelegramAdapter::format_markdown_v2("[link](url)");
    EXPECT_EQ(result, "\\[link\\]\\(url\\)");
}

TEST(TelegramAdapter, SendReturnsTrue) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true,"result":{"message_id":1}})", {}});

    TelegramAdapter::Config cfg;
    cfg.bot_token = "token";
    TelegramAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.send("123", "hello"));

    // Verify the request was made to the correct URL.
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/sendMessage"), std::string::npos);
}

TEST(TelegramAdapter, SendTypingMakesRequest) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true})", {}});

    TelegramAdapter::Config cfg;
    cfg.bot_token = "token";
    TelegramAdapter adapter(cfg, &fake);
    adapter.send_typing("123");

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/sendChatAction"), std::string::npos);
}

// --- Discord ---

TEST(DiscordAdapter, ConnectSucceeds) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"id":"123","username":"bot"})", {}});

    DiscordAdapter::Config cfg;
    cfg.bot_token = "discord-bot-token";
    DiscordAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.connect());

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/users/@me"), std::string::npos);
}

TEST(DiscordAdapter, ConnectEmptyTokenFails) {
    DiscordAdapter::Config cfg;
    DiscordAdapter adapter(cfg);
    EXPECT_FALSE(adapter.connect());
}

TEST(DiscordAdapter, PlatformReturnsDiscord) {
    DiscordAdapter::Config cfg;
    DiscordAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Discord);
}

TEST(DiscordAdapter, MentionFormat) {
    EXPECT_EQ(DiscordAdapter::format_mention("123456"), "<@123456>");
}

TEST(DiscordAdapter, SendMakesRequest) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"id":"1"})", {}});

    DiscordAdapter::Config cfg;
    cfg.bot_token = "token";
    DiscordAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.send("channel123", "hello world"));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/channels/channel123/messages"),
              std::string::npos);
}

// --- Slack ---

TEST(SlackAdapter, ConnectSucceeds) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true,"user_id":"U123"})", {}});

    SlackAdapter::Config cfg;
    cfg.bot_token = "xoxb-test-token";
    SlackAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.connect());

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("auth.test"), std::string::npos);
}

TEST(SlackAdapter, SendMakesRequest) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true,"ts":"123.456"})", {}});

    SlackAdapter::Config cfg;
    cfg.bot_token = "xoxb-test";
    SlackAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.send("C12345", "hello"));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("chat.postMessage"), std::string::npos);
}

TEST(SlackAdapter, PlatformReturnsSlack) {
    SlackAdapter::Config cfg;
    SlackAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Slack);
}

TEST(SlackAdapter, HmacSignatureComputation) {
    // Known test vector: Slack's v0= prefix + HMAC-SHA256.
    auto sig = SlackAdapter::compute_slack_signature("secret", "1234567890", "body");
    EXPECT_FALSE(sig.empty());
    EXPECT_EQ(sig.substr(0, 3), "v0=");
    // Length: "v0=" + 64 hex chars = 67
    EXPECT_EQ(sig.size(), 67u);
}

TEST(SlackAdapter, HmacSignatureDeterministic) {
    auto s1 = SlackAdapter::compute_slack_signature("key", "ts", "data");
    auto s2 = SlackAdapter::compute_slack_signature("key", "ts", "data");
    EXPECT_EQ(s1, s2);
}

// --- ApiServer ---

TEST(ApiServerAdapter, PlatformReturnsApiServer) {
    ApiServerAdapter::Config cfg;
    ApiServerAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::ApiServer);
}

TEST(ApiServerAdapter, ConnectAlwaysSucceeds) {
    ApiServerAdapter::Config cfg;
    ApiServerAdapter adapter(cfg);
    EXPECT_TRUE(adapter.connect());
    EXPECT_TRUE(adapter.connected());
}

TEST(ApiServerAdapter, SendStoresResponse) {
    ApiServerAdapter::Config cfg;
    ApiServerAdapter adapter(cfg);
    EXPECT_TRUE(adapter.send("req-1", "hello response"));
    EXPECT_EQ(adapter.get_pending_response("req-1"), "hello response");
    // Second call should return empty (consumed).
    EXPECT_TRUE(adapter.get_pending_response("req-1").empty());
}

TEST(ApiServerAdapter, HmacVerification) {
    std::string secret = "test-secret";
    std::string body = "test-body";
    EXPECT_FALSE(ApiServerAdapter::verify_hmac_signature(secret, body, "wrong"));
}

TEST(ApiServerAdapter, HmacVerifySameInput) {
    std::string secret = "key123";
    std::string body = R"({"message":"hello"})";
    bool r1 = ApiServerAdapter::verify_hmac_signature(secret, body, "abc");
    bool r2 = ApiServerAdapter::verify_hmac_signature(secret, body, "abc");
    EXPECT_EQ(r1, r2);
}

// --- Webhook ---

TEST(WebhookAdapter, PlatformReturnsWebhook) {
    WebhookAdapter::Config cfg;
    WebhookAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Webhook);
}

TEST(WebhookAdapter, ConnectAlwaysSucceeds) {
    WebhookAdapter::Config cfg;
    WebhookAdapter adapter(cfg);
    EXPECT_TRUE(adapter.connect());
}

TEST(WebhookAdapter, SendPostsToEndpoint) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "{}", {}});

    WebhookAdapter::Config cfg;
    cfg.endpoint_url = "https://example.com/hook";
    cfg.signature_secret = "mysecret";
    WebhookAdapter adapter(cfg, &fake);
    EXPECT_TRUE(adapter.send("ch1", "payload"));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_EQ(fake.requests()[0].url, "https://example.com/hook");
    // Should have signature header.
    auto it = fake.requests()[0].headers.find("X-Hermes-Signature");
    EXPECT_NE(it, fake.requests()[0].headers.end());
}

// --- Weixin ---

TEST(WeixinAdapter, PlatformReturnsWeixin) {
    WeixinAdapter::Config cfg;
    WeixinAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Weixin);
}

TEST(WeixinAdapter, ParseXmlMessageCdata) {
    std::string xml = R"(<xml>
<ToUserName><![CDATA[toUser]]></ToUserName>
<FromUserName><![CDATA[fromUser]]></FromUserName>
<MsgType><![CDATA[text]]></MsgType>
<Content><![CDATA[Hello World]]></Content>
<MsgId>1234567890</MsgId>
</xml>)";

    auto evt = WeixinAdapter::parse_xml_message(xml);
    EXPECT_EQ(evt.from_user, "fromUser");
    EXPECT_EQ(evt.to_user, "toUser");
    EXPECT_EQ(evt.msg_type, "text");
    EXPECT_EQ(evt.content, "Hello World");
    EXPECT_EQ(evt.msg_id, "1234567890");
}

TEST(WeixinAdapter, ParseXmlMessagePlain) {
    std::string xml = "<xml><FromUserName>u1</FromUserName>"
                      "<ToUserName>u2</ToUserName>"
                      "<MsgType>text</MsgType>"
                      "<Content>hi</Content>"
                      "<MsgId>99</MsgId></xml>";

    auto evt = WeixinAdapter::parse_xml_message(xml);
    EXPECT_EQ(evt.from_user, "u1");
    EXPECT_EQ(evt.content, "hi");
}

// --- Email ---

TEST(EmailAdapter, PlatformReturnsEmail) {
    EmailAdapter::Config cfg;
    EmailAdapter adapter(cfg);
    EXPECT_EQ(adapter.platform(), Platform::Email);
}

TEST(EmailAdapter, MimeMessageConstruction) {
    auto mime = EmailAdapter::build_mime_message(
        "from@example.com", "to@example.com", "Test Subject", "Hello body");
    EXPECT_NE(mime.find("From: from@example.com"), std::string::npos);
    EXPECT_NE(mime.find("To: to@example.com"), std::string::npos);
    EXPECT_NE(mime.find("Subject: Test Subject"), std::string::npos);
    EXPECT_NE(mime.find("MIME-Version: 1.0"), std::string::npos);
    EXPECT_NE(mime.find("Content-Type: text/plain"), std::string::npos);
    EXPECT_NE(mime.find("Hello body"), std::string::npos);
}

// --- Local ---

TEST(LocalAdapter, PlatformReturnsLocal) {
    LocalAdapter adapter;
    EXPECT_EQ(adapter.platform(), Platform::Local);
}

TEST(LocalAdapter, ConnectSucceeds) {
    LocalAdapter adapter;
    EXPECT_TRUE(adapter.connect());
}

TEST(LocalAdapter, SendWritesToStdout) {
    LocalAdapter adapter;
    // Capture stdout.
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    EXPECT_TRUE(adapter.send("test", "hello stdout"));
    std::cout.rdbuf(old);
    EXPECT_NE(oss.str().find("hello stdout"), std::string::npos);
}

// --- Factory ---

TEST(AdapterFactory, AvailableAdaptersReturns18) {
    auto adapters = available_adapters();
    EXPECT_EQ(adapters.size(), 18u);
}

TEST(AdapterFactory, CreateAllAdapters) {
    PlatformConfig cfg;
    cfg.token = "test-token";
    cfg.extra = nlohmann::json{
        {"session_dir", "/tmp"},
        {"phone", "+1234"},
        {"http_url", "http://localhost"},
        {"account", "acc"},
        {"homeserver", "https://matrix.org"},
        {"username", "user"},
        {"password", "pass"},
        {"url", "https://mm.example.com"},
        {"address", "user@example.com"},
        {"imap_host", "imap.example.com"},
        {"smtp_host", "smtp.example.com"},
        {"twilio_account_sid", "sid"},
        {"twilio_auth_token", "auth"},
        {"client_id", "cid"},
        {"client_secret", "csec"},
        {"app_id", "aid"},
        {"app_secret", "asec"},
        {"bot_id", "bid"},
        {"message_token", "mt"},
        {"webhook_url", "https://hook"},
        {"appid", "wxid"},
        {"appsecret", "wxsec"},
        {"server_url", "http://bb"},
        {"hmac_secret", "hmac"},
        {"signature_secret", "sig"},
        {"endpoint_url", "http://ep"},
    };

    auto all = available_adapters();
    for (auto p : all) {
        auto adapter = create_adapter(p, cfg);
        ASSERT_NE(adapter, nullptr) << "Failed for platform "
                                     << static_cast<int>(p);
        EXPECT_EQ(adapter->platform(), p);
    }
}

TEST(AdapterFactory, CreateLocalWithEmptyConfig) {
    PlatformConfig cfg;
    auto adapter = create_adapter(Platform::Local, cfg);
    ASSERT_NE(adapter, nullptr);
    EXPECT_EQ(adapter->platform(), Platform::Local);
    EXPECT_TRUE(adapter->connect());
}
