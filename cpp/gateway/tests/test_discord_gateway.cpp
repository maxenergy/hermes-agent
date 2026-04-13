// Unit tests for the Discord gateway (v10) protocol driver.
//
// Uses the mock WebSocketTransport to drive the opcode state machine
// without network. Asserts on frame shapes (identify/heartbeat/resume),
// session tracking, and MESSAGE_CREATE dispatch.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../platforms/discord.hpp"
#include "../platforms/discord_gateway.hpp"
#include "../platforms/websocket_transport.hpp"

using hermes::gateway::platforms::DiscordAdapter;
using hermes::gateway::platforms::DiscordGateway;
using hermes::gateway::platforms::WebSocketTransport;
using hermes::gateway::platforms::make_mock_websocket_transport;
using hermes::gateway::platforms::mock_drain_sent;
using hermes::gateway::platforms::mock_inject_inbound;
using hermes::gateway::platforms::mock_last_host;
using hermes::gateway::platforms::mock_last_path;

namespace {

nlohmann::json parse_first_sent(WebSocketTransport* t) {
    auto sent = mock_drain_sent(t);
    if (sent.empty()) return nlohmann::json::object();
    return nlohmann::json::parse(sent.front());
}

}  // namespace

TEST(DiscordGateway, IdentifyPayloadShape) {
    DiscordGateway::Config cfg;
    cfg.token = "BOT_TOKEN";
    cfg.intents = (1 << 0) | (1 << 9) | (1 << 15);

    auto frame = DiscordGateway::build_identify_payload(cfg);
    EXPECT_EQ(frame["op"].get<int>(), DiscordGateway::OP_IDENTIFY);
    auto& d = frame["d"];
    EXPECT_EQ(d["token"].get<std::string>(), "BOT_TOKEN");
    EXPECT_EQ(d["intents"].get<int>(), cfg.intents);
    ASSERT_TRUE(d.contains("properties"));
    EXPECT_TRUE(d["properties"].contains("os"));
    EXPECT_TRUE(d["properties"].contains("browser"));
    EXPECT_TRUE(d["properties"].contains("device"));
    EXPECT_FALSE(d["compress"].get<bool>());
}

TEST(DiscordGateway, ResumePayloadShape) {
    auto frame = DiscordGateway::build_resume_payload("TKN", "SESSION_X", 42);
    EXPECT_EQ(frame["op"].get<int>(), DiscordGateway::OP_RESUME);
    auto& d = frame["d"];
    EXPECT_EQ(d["token"].get<std::string>(), "TKN");
    EXPECT_EQ(d["session_id"].get<std::string>(), "SESSION_X");
    EXPECT_EQ(d["seq"].get<std::int64_t>(), 42);
}

TEST(DiscordGateway, HelloTriggersIdentifyAndRecordsInterval) {
    DiscordGateway::Config cfg;
    cfg.token = "BOT";
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));

    // Prime HELLO before connect() polls.
    nlohmann::json hello = {
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 41250}}}
    };
    mock_inject_inbound(raw, hello.dump());

    ASSERT_TRUE(gw.connect());
    EXPECT_EQ(gw.heartbeat_interval_ms(), 41250);

    // After connect, we should have sent IDENTIFY.
    auto frame = parse_first_sent(raw);
    EXPECT_EQ(frame["op"].get<int>(), DiscordGateway::OP_IDENTIFY);

    // Connected to the correct gateway URL.
    EXPECT_EQ(mock_last_host(raw), "gateway.discord.gg");
    EXPECT_NE(mock_last_path(raw).find("v=10"), std::string::npos);
    EXPECT_NE(mock_last_path(raw).find("encoding=json"), std::string::npos);
}

TEST(DiscordGateway, ReadyRecordsSessionId) {
    DiscordGateway::Config cfg;
    cfg.token = "BOT";
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));

    mock_inject_inbound(raw, nlohmann::json{
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 45000}}}
    }.dump());
    ASSERT_TRUE(gw.connect());

    // Drain IDENTIFY frame.
    (void)mock_drain_sent(raw);

    // Inject READY (DISPATCH op 0) with session info.
    nlohmann::json ready = {
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "READY"},
        {"s", 1},
        {"d", {
            {"session_id", "SESS_ABC"},
            {"resume_gateway_url", "wss://resume.discord.gg"}
        }}
    };
    mock_inject_inbound(raw, ready.dump());
    EXPECT_TRUE(gw.run_once());

    EXPECT_EQ(gw.session_id(), "SESS_ABC");
    EXPECT_EQ(gw.last_sequence(), 1);
    EXPECT_TRUE(gw.ready());
}

TEST(DiscordGateway, HeartbeatSendsOpcode1WithSequence) {
    DiscordGateway::Config cfg;
    cfg.token = "BOT";
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));

    // Manually drive without calling connect() so heartbeat_interval is
    // controlled.
    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 45000}}}
    }.dump());

    // Record a sequence via a fake DISPATCH.
    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "TYPING_START"},
        {"s", 7},
        {"d", nlohmann::json::object()}
    }.dump());
    (void)mock_drain_sent(raw);

    // The mock transport is not yet "connected" — open it explicitly.
    raw->connect("h", "443", "/");
    ASSERT_TRUE(gw.send_heartbeat());
    auto frame = parse_first_sent(raw);
    EXPECT_EQ(frame["op"].get<int>(), DiscordGateway::OP_HEARTBEAT);
    EXPECT_EQ(frame["d"].get<std::int64_t>(), 7);
}

TEST(DiscordGateway, HeartbeatAckClearsPendingFlag) {
    DiscordGateway::Config cfg;
    cfg.token = "BOT";
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));
    raw->connect("h", "443", "/");

    // HELLO -> interval known, then synthesize heartbeat.
    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 45000}}}
    }.dump());
    gw.send_heartbeat();
    EXPECT_FALSE(gw.heartbeat_ack_received());

    // Server ACK.
    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_HEARTBEAT_ACK}
    }.dump());
    EXPECT_TRUE(gw.heartbeat_ack_received());
}

TEST(DiscordGateway, ResumeFrameUsesLastSessionAndSeq) {
    DiscordGateway::Config cfg;
    cfg.token = "TKN";
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));
    raw->connect("h", "443", "/");

    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "READY"},
        {"s", 99},
        {"d", {{"session_id", "SESS_99"}}}
    }.dump());

    ASSERT_TRUE(gw.resume());
    auto frame = parse_first_sent(raw);
    EXPECT_EQ(frame["op"].get<int>(), DiscordGateway::OP_RESUME);
    EXPECT_EQ(frame["d"]["session_id"].get<std::string>(), "SESS_99");
    EXPECT_EQ(frame["d"]["seq"].get<std::int64_t>(), 99);
    EXPECT_EQ(frame["d"]["token"].get<std::string>(), "TKN");
}

TEST(DiscordGateway, InvalidSessionNonResumableClearsState) {
    DiscordGateway::Config cfg;
    DiscordGateway gw(cfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    gw.set_transport(std::move(transport));
    raw->connect("h", "443", "/");

    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "READY"},
        {"s", 5},
        {"d", {{"session_id", "old"}}}
    }.dump());
    EXPECT_EQ(gw.session_id(), "old");

    gw.handle_frame(nlohmann::json{
        {"op", DiscordGateway::OP_INVALID_SESSION},
        {"d", false}
    }.dump());
    EXPECT_EQ(gw.session_id(), "");
    EXPECT_EQ(gw.last_sequence(), -1);
}

TEST(DiscordGateway, MessageCreateReachesAdapterCallback) {
    DiscordAdapter::Config acfg;
    acfg.bot_token = "BOT";
    DiscordAdapter adapter(acfg);

    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    adapter.configure_gateway(/*intents=*/0, std::move(transport));

    std::string seen_channel, seen_user, seen_content, seen_id;
    adapter.set_message_callback(
        [&](const std::string& c, const std::string& u,
            const std::string& t, const std::string& m) {
            seen_channel = c;
            seen_user = u;
            seen_content = t;
            seen_id = m;
        });

    // Prime HELLO so connect() completes.
    mock_inject_inbound(raw, nlohmann::json{
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 45000}}}
    }.dump());
    ASSERT_TRUE(adapter.start_gateway());

    // Deliver a MESSAGE_CREATE dispatch.
    nlohmann::json mc = {
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "MESSAGE_CREATE"},
        {"s", 2},
        {"d", {
            {"id", "MSG1"},
            {"channel_id", "CHAN1"},
            {"content", "hello"},
            {"author", {
                {"id", "USER1"},
                {"bot", false}
            }}
        }}
    };
    mock_inject_inbound(raw, mc.dump());
    EXPECT_TRUE(adapter.gateway_run_once());

    EXPECT_EQ(seen_channel, "CHAN1");
    EXPECT_EQ(seen_user, "USER1");
    EXPECT_EQ(seen_content, "hello");
    EXPECT_EQ(seen_id, "MSG1");
}

TEST(DiscordGateway, MessageCreateFromBotIsSuppressed) {
    DiscordAdapter::Config acfg;
    acfg.bot_token = "BOT";
    DiscordAdapter adapter(acfg);
    auto transport = make_mock_websocket_transport();
    auto* raw = transport.get();
    adapter.configure_gateway(0, std::move(transport));

    int calls = 0;
    adapter.set_message_callback(
        [&](const std::string&, const std::string&,
            const std::string&, const std::string&) { ++calls; });

    mock_inject_inbound(raw, nlohmann::json{
        {"op", DiscordGateway::OP_HELLO},
        {"d", {{"heartbeat_interval", 45000}}}
    }.dump());
    ASSERT_TRUE(adapter.start_gateway());

    mock_inject_inbound(raw, nlohmann::json{
        {"op", DiscordGateway::OP_DISPATCH},
        {"t", "MESSAGE_CREATE"},
        {"s", 2},
        {"d", {
            {"id", "MSG2"},
            {"channel_id", "CHAN1"},
            {"content", "echo"},
            {"author", {{"id", "BOT"}, {"bot", true}}}
        }}
    }.dump());
    EXPECT_TRUE(adapter.gateway_run_once());
    EXPECT_EQ(calls, 0);
}
