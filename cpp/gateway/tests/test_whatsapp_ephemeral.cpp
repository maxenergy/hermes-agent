// Phase 12.5 — WhatsApp disappearing-message + group v2 tests.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../platforms/whatsapp.hpp"

#include <hermes/llm/llm_client.hpp>

using hermes::gateway::platforms::WhatsAppAdapter;
using hermes::llm::FakeHttpTransport;

namespace {
WhatsAppAdapter::Config graph_cfg() {
    WhatsAppAdapter::Config c;
    c.session_dir = "/tmp/hermes-wa-eph";
    c.phone = "PHONE_NUMBER_ID_123";
    return c;
}

WhatsAppAdapter::Config bridge_cfg() {
    WhatsAppAdapter::Config c;
    c.session_dir = "/tmp/hermes-wa-eph";
    c.phone = "+12125551234";
    c.bridge_url = "http://127.0.0.1:7770";
    return c;
}
}  // namespace

TEST(WhatsAppEphemeral, ParseDurationCamelCase) {
    auto j = nlohmann::json::parse(R"({"ephemeralDuration":86400})");
    EXPECT_EQ(WhatsAppAdapter::parse_ephemeral_duration(j), 86400);
}

TEST(WhatsAppEphemeral, ParseDurationSnakeCase) {
    auto j = nlohmann::json::parse(R"({"ephemeral_expiration":604800})");
    EXPECT_EQ(WhatsAppAdapter::parse_ephemeral_duration(j), 604800);
}

TEST(WhatsAppEphemeral, ParseDurationNestedMessage) {
    auto j = nlohmann::json::parse(
        R"({"message":{"ephemeralDuration":7776000}})");
    EXPECT_EQ(WhatsAppAdapter::parse_ephemeral_duration(j), 7776000);
}

TEST(WhatsAppEphemeral, ParseDurationContextInfo) {
    auto j = nlohmann::json::parse(
        R"({"messageContextInfo":{"expiration":3600}})");
    EXPECT_EQ(WhatsAppAdapter::parse_ephemeral_duration(j), 3600);
}

TEST(WhatsAppEphemeral, ParseDurationAbsent) {
    auto j = nlohmann::json::parse(R"({"text":"hi"})");
    EXPECT_EQ(WhatsAppAdapter::parse_ephemeral_duration(j), 0);
}

TEST(WhatsAppEphemeral, SendWithEphemeralIncludesFieldGraph) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"messages":[{"id":"x"}]})", {}});

    WhatsAppAdapter adapter(graph_cfg(), &fake);
    EXPECT_TRUE(adapter.send_with_ephemeral("12125551234", "hi", 86400));

    ASSERT_EQ(fake.requests().size(), 1u);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("ephemeral_expiration"));
    EXPECT_EQ(body["ephemeral_expiration"].get<int>(), 86400);
    EXPECT_EQ(body["messaging_product"].get<std::string>(), "whatsapp");
    EXPECT_EQ(body["to"].get<std::string>(), "12125551234@s.whatsapp.net");
}

TEST(WhatsAppEphemeral, SendWithoutTimerOmitsField) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"messages":[{"id":"x"}]})", {}});

    WhatsAppAdapter adapter(graph_cfg(), &fake);
    EXPECT_TRUE(adapter.send("12125551234", "hi"));

    ASSERT_EQ(fake.requests().size(), 1u);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_FALSE(body.contains("ephemeral_expiration"));
}

TEST(WhatsAppEphemeral, SendHonoursRememberedTimer) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"messages":[{"id":"x"}]})", {}});

    WhatsAppAdapter adapter(graph_cfg(), &fake);
    adapter.remember_ephemeral("12125551234", 604800);
    EXPECT_EQ(adapter.ephemeral_for("12125551234"), 604800);

    EXPECT_TRUE(adapter.send("12125551234", "hi"));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["ephemeral_expiration"].get<int>(), 604800);
}

TEST(WhatsAppEphemeral, RememberZeroClears) {
    WhatsAppAdapter adapter(graph_cfg());
    adapter.remember_ephemeral("abc", 60);
    EXPECT_EQ(adapter.ephemeral_for("abc"), 60);
    adapter.remember_ephemeral("abc", 0);
    EXPECT_EQ(adapter.ephemeral_for("abc"), 0);
}

TEST(WhatsAppEphemeral, SendUsesBridgeEndpointWhenConfigured) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"status":"queued"})", {}});

    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    EXPECT_TRUE(adapter.send_with_ephemeral("12125551234", "hi", 86400));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/send"), std::string::npos);
    EXPECT_EQ(fake.requests()[0].url.find("graph.facebook.com"),
              std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["ephemeral_expiration"].get<int>(), 86400);
}

// Group v2.

TEST(WhatsAppGroupV2, ParseParticipantAdd) {
    auto j = nlohmann::json::parse(R"({
        "type": "group",
        "action": "add",
        "group": "120363020000000000@g.us",
        "participants": ["12125551234@s.whatsapp.net", "abcdef@lid"]
    })");
    auto ev = WhatsAppAdapter::parse_group_event(j);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, WhatsAppAdapter::GroupEventType::ParticipantsAdd);
    EXPECT_EQ(ev->group_id, "120363020000000000@g.us");
    ASSERT_EQ(ev->participants.size(), 2u);
    EXPECT_EQ(ev->participants[0], "12125551234@s.whatsapp.net");
    EXPECT_EQ(ev->participants[1], "abcdef@lid");
}

TEST(WhatsAppGroupV2, ParseParticipantRemove) {
    auto j = nlohmann::json::parse(R"({
        "type": "group",
        "action": "remove",
        "group": "g@g.us",
        "participants": [{"id": "u@lid"}]
    })");
    auto ev = WhatsAppAdapter::parse_group_event(j);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, WhatsAppAdapter::GroupEventType::ParticipantsRemove);
    ASSERT_EQ(ev->participants.size(), 1u);
    EXPECT_EQ(ev->participants[0], "u@lid");
}

TEST(WhatsAppGroupV2, ParseAdminPromoteDemote) {
    auto j = nlohmann::json::parse(R"({
        "type": "group", "action": "promote", "group": "g@g.us",
        "participants": ["u@lid"]
    })");
    auto ev = WhatsAppAdapter::parse_group_event(j);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, WhatsAppAdapter::GroupEventType::AdminPromote);

    auto j2 = nlohmann::json::parse(R"({
        "type": "group", "action": "demote", "group": "g@g.us",
        "participants": ["u@lid"]
    })");
    auto ev2 = WhatsAppAdapter::parse_group_event(j2);
    ASSERT_TRUE(ev2.has_value());
    EXPECT_EQ(ev2->type, WhatsAppAdapter::GroupEventType::AdminDemote);
}

TEST(WhatsAppGroupV2, ParseEphemeralChange) {
    auto j = nlohmann::json::parse(R"({
        "type": "group", "action": "ephemeral", "group": "g@g.us",
        "ephemeral": 604800
    })");
    auto ev = WhatsAppAdapter::parse_group_event(j);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, WhatsAppAdapter::GroupEventType::EphemeralChange);
    EXPECT_EQ(ev->ephemeral_expiration_sec, 604800);
}

TEST(WhatsAppGroupV2, ParseRejectsUnknownAction) {
    auto j = nlohmann::json::parse(
        R"({"type":"group","action":"weirdo","group":"g@g.us"})");
    EXPECT_FALSE(WhatsAppAdapter::parse_group_event(j).has_value());
}

TEST(WhatsAppGroupV2, ParseRejectsMissingGroup) {
    auto j = nlohmann::json::parse(R"({"type":"group","action":"add"})");
    EXPECT_FALSE(WhatsAppAdapter::parse_group_event(j).has_value());
}

TEST(WhatsAppGroupV2, ParseRejectsUnrelated) {
    auto j = nlohmann::json::parse(R"({"type":"text","body":"hello"})");
    EXPECT_FALSE(WhatsAppAdapter::parse_group_event(j).has_value());
}
