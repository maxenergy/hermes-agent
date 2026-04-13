// Unit tests for the Slack Socket Mode / RTM WebSocket driver.
//
// Uses the mock WebSocketTransport to validate envelope ACK behavior,
// RTM ping/pong, URL parsing, and event routing through the adapter.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../platforms/slack.hpp"
#include "../platforms/slack_socket_mode.hpp"
#include "../platforms/websocket_transport.hpp"

using hermes::gateway::platforms::SlackAdapter;
using hermes::gateway::platforms::SlackSocketMode;
using hermes::gateway::platforms::WebSocketTransport;
using hermes::gateway::platforms::make_mock_websocket_transport;
using hermes::gateway::platforms::mock_drain_sent;
using hermes::gateway::platforms::mock_inject_inbound;

TEST(SlackSocketMode, ParsesWssUrl) {
    std::string host, port, path;
    ASSERT_TRUE(SlackSocketMode::parse_wss_url(
        "wss://wss-primary.slack.com/link/?ticket=abc&app_id=xy",
        host, port, path));
    EXPECT_EQ(host, "wss-primary.slack.com");
    EXPECT_EQ(port, "443");
    EXPECT_EQ(path, "/link/?ticket=abc&app_id=xy");

    ASSERT_TRUE(SlackSocketMode::parse_wss_url(
        "wss://host.example:9443/realtime",
        host, port, path));
    EXPECT_EQ(host, "host.example");
    EXPECT_EQ(port, "9443");
    EXPECT_EQ(path, "/realtime");

    EXPECT_FALSE(SlackSocketMode::parse_wss_url(
        "https://not-a-websocket.example/", host, port, path));
}

TEST(SlackSocketMode, DetectsSocketModeFromXappToken) {
    SlackSocketMode::Config cfg;
    cfg.app_token = "xapp-1-A0";
    cfg.ws_url = "wss://a.b/";
    SlackSocketMode sm(cfg);
    EXPECT_EQ(sm.mode(), SlackSocketMode::Mode::SocketMode);
}

TEST(SlackSocketMode, DetectsRtmFromBotToken) {
    SlackSocketMode::Config cfg;
    cfg.bot_token = "xoxb-XYZ";
    cfg.ws_url = "wss://a.b/";
    SlackSocketMode sm(cfg);
    EXPECT_EQ(sm.mode(), SlackSocketMode::Mode::RTM);
}

TEST(SlackSocketMode, SocketModeAutoAcksEnvelope) {
    SlackSocketMode::Config cfg;
    cfg.app_token = "xapp-1-A0";
    cfg.ws_url = "wss://wss-primary.slack.com/link/?ticket=abc";
    SlackSocketMode sm(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    sm.set_transport(std::move(transport));
    ASSERT_TRUE(sm.connect());

    std::string seen_type;
    nlohmann::json seen_payload;
    sm.set_event_callback(
        [&](const std::string& t, const nlohmann::json& d) {
            seen_type = t;
            seen_payload = d;
        });

    nlohmann::json envelope = {
        {"type", "events_api"},
        {"envelope_id", "abc-123"},
        {"payload", {
            {"event", {
                {"type", "message"},
                {"channel", "C1"},
                {"user", "U1"},
                {"text", "hi"},
                {"ts", "1.0"}
            }}
        }}
    };
    mock_inject_inbound(raw, envelope.dump());
    EXPECT_TRUE(sm.run_once());

    auto sent = mock_drain_sent(raw);
    ASSERT_EQ(sent.size(), 1u);
    auto ack = nlohmann::json::parse(sent.front());
    EXPECT_EQ(ack["envelope_id"].get<std::string>(), "abc-123");

    EXPECT_EQ(seen_type, "events_api");
    ASSERT_TRUE(seen_payload.contains("event"));
    EXPECT_EQ(seen_payload["event"]["text"].get<std::string>(), "hi");
}

TEST(SlackSocketMode, SocketModeHelloFrameDoesNotAck) {
    SlackSocketMode::Config cfg;
    cfg.app_token = "xapp-1-A0";
    cfg.ws_url = "wss://a.b/";
    SlackSocketMode sm(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    sm.set_transport(std::move(transport));
    ASSERT_TRUE(sm.connect());

    mock_inject_inbound(raw, R"({"type":"hello","num_connections":1})");
    EXPECT_TRUE(sm.run_once());

    auto sent = mock_drain_sent(raw);
    EXPECT_TRUE(sent.empty());
}

TEST(SlackSocketMode, RtmPingAutoReplies) {
    SlackSocketMode::Config cfg;
    cfg.bot_token = "xoxb-BOT";
    cfg.ws_url = "wss://rtm.slack.com/x";
    SlackSocketMode sm(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    sm.set_transport(std::move(transport));
    ASSERT_TRUE(sm.connect());

    mock_inject_inbound(raw, R"({"type":"ping","id":42})");
    EXPECT_TRUE(sm.run_once());

    auto sent = mock_drain_sent(raw);
    ASSERT_EQ(sent.size(), 1u);
    auto pong = nlohmann::json::parse(sent.front());
    EXPECT_EQ(pong["type"].get<std::string>(), "pong");
    EXPECT_EQ(pong["id"].get<int>(), 42);
}

TEST(SlackAdapter, RealtimeRoutesMessageEventToCallback) {
    SlackAdapter::Config acfg;
    acfg.app_token = "xapp-1-A0";
    SlackAdapter adapter(acfg);

    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    adapter.configure_realtime("wss://wss-primary.slack.com/link/?t=1",
                               std::move(transport));

    std::string seen_channel, seen_user, seen_text, seen_ts;
    adapter.set_message_callback(
        [&](const std::string& c, const std::string& u,
            const std::string& t, const std::string& ts) {
            seen_channel = c;
            seen_user = u;
            seen_text = t;
            seen_ts = ts;
        });

    ASSERT_TRUE(adapter.start_realtime());

    nlohmann::json envelope = {
        {"type", "events_api"},
        {"envelope_id", "env-1"},
        {"payload", {
            {"event", {
                {"type", "message"},
                {"channel", "C9"},
                {"user", "U9"},
                {"text", "hello slack"},
                {"ts", "1698765432.000100"}
            }}
        }}
    };
    mock_inject_inbound(raw, envelope.dump());
    EXPECT_TRUE(adapter.realtime_run_once());

    EXPECT_EQ(seen_channel, "C9");
    EXPECT_EQ(seen_user, "U9");
    EXPECT_EQ(seen_text, "hello slack");
    EXPECT_EQ(seen_ts, "1698765432.000100");
}

TEST(SlackAdapter, RealtimeSuppressesBotAndSubtypeMessages) {
    SlackAdapter::Config acfg;
    acfg.app_token = "xapp-1-A0";
    SlackAdapter adapter(acfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    adapter.configure_realtime("wss://a.b/", std::move(transport));

    int calls = 0;
    adapter.set_message_callback(
        [&](const std::string&, const std::string&,
            const std::string&, const std::string&) { ++calls; });

    ASSERT_TRUE(adapter.start_realtime());

    // Bot message: has bot_id → suppressed.
    mock_inject_inbound(raw, nlohmann::json{
        {"type", "events_api"},
        {"envelope_id", "e1"},
        {"payload", {
            {"event", {
                {"type", "message"},
                {"bot_id", "B1"},
                {"channel", "C"}, {"user", ""}, {"text", "x"}, {"ts", "1.0"}
            }}
        }}
    }.dump());
    EXPECT_TRUE(adapter.realtime_run_once());

    // Edited message: has subtype → suppressed.
    mock_inject_inbound(raw, nlohmann::json{
        {"type", "events_api"},
        {"envelope_id", "e2"},
        {"payload", {
            {"event", {
                {"type", "message"},
                {"subtype", "message_changed"},
                {"channel", "C"}, {"user", "U"}, {"text", "x"}, {"ts", "1.0"}
            }}
        }}
    }.dump());
    EXPECT_TRUE(adapter.realtime_run_once());

    EXPECT_EQ(calls, 0);
}
