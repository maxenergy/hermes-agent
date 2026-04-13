// Phase 12.5 — WhatsApp phone+code pairing tests.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../platforms/whatsapp.hpp"

#include <hermes/llm/llm_client.hpp>

using hermes::gateway::platforms::WhatsAppAdapter;
using hermes::llm::FakeHttpTransport;

namespace {
WhatsAppAdapter::Config bridge_cfg() {
    WhatsAppAdapter::Config c;
    c.session_dir = "/tmp/hermes-wa-test";
    c.phone = "+12125551234";
    c.bridge_url = "http://127.0.0.1:7770";
    return c;
}
}  // namespace

TEST(WhatsAppPairing, StartPairingReturnsCodeFromBridge) {
    FakeHttpTransport fake;
    fake.enqueue_response({200,
                           R"({"code":"ABCD-EFGH","phone":"+12125551234","expires_in":60})",
                           {}});

    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    auto pc = adapter.start_pairing("+12125551234");
    ASSERT_TRUE(pc.has_value());
    EXPECT_EQ(pc->code, "ABCD-EFGH");
    EXPECT_EQ(pc->phone, "+12125551234");
    EXPECT_EQ(pc->expires_in_seconds, 60);

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/pair"), std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["phone"].get<std::string>(), "+12125551234");
}

TEST(WhatsAppPairing, StartPairingFailsWithoutBridge) {
    FakeHttpTransport fake;
    WhatsAppAdapter::Config c = bridge_cfg();
    c.bridge_url.clear();
    WhatsAppAdapter adapter(c, &fake);
    auto pc = adapter.start_pairing("+12125551234");
    EXPECT_FALSE(pc.has_value());
    EXPECT_TRUE(fake.requests().empty());
}

TEST(WhatsAppPairing, StartPairingFailsWithEmptyPhone) {
    FakeHttpTransport fake;
    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    EXPECT_FALSE(adapter.start_pairing("").has_value());
    EXPECT_TRUE(fake.requests().empty());
}

TEST(WhatsAppPairing, StartPairingFailsOnNon2xx) {
    FakeHttpTransport fake;
    fake.enqueue_response({500, R"({"error":"internal"})", {}});
    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    EXPECT_FALSE(adapter.start_pairing("+12125551234").has_value());
}

TEST(WhatsAppPairing, StartPairingFailsOnMalformedJson) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "not-json", {}});
    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    EXPECT_FALSE(adapter.start_pairing("+12125551234").has_value());
}

TEST(WhatsAppPairing, StartPairingFailsWhenCodeMissing) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"phone":"+12125551234"})", {}});
    WhatsAppAdapter adapter(bridge_cfg(), &fake);
    EXPECT_FALSE(adapter.start_pairing("+12125551234").has_value());
}

TEST(WhatsAppPairing, ParticipantClassification) {
    using K = WhatsAppAdapter::ParticipantKind;
    EXPECT_EQ(WhatsAppAdapter::classify_participant("12125551234@s.whatsapp.net"),
              K::LegacyJid);
    EXPECT_EQ(WhatsAppAdapter::classify_participant("abc123@lid"), K::Lid);
    EXPECT_EQ(WhatsAppAdapter::classify_participant("g1234567@g.us"),
              K::GroupJid);
    EXPECT_EQ(WhatsAppAdapter::classify_participant("x@broadcast"),
              K::Broadcast);
    EXPECT_EQ(WhatsAppAdapter::classify_participant("12125551234"),
              K::LegacyJid);
    EXPECT_EQ(WhatsAppAdapter::classify_participant(""), K::Unknown);
    EXPECT_EQ(WhatsAppAdapter::classify_participant("foo@bar"), K::Unknown);
}

TEST(WhatsAppPairing, ResolveJidPassthroughsQualified) {
    EXPECT_EQ(WhatsAppAdapter::resolve_jid("12125551234"),
              "12125551234@s.whatsapp.net");
    EXPECT_EQ(WhatsAppAdapter::resolve_jid("abc@lid"), "abc@lid");
    EXPECT_EQ(WhatsAppAdapter::resolve_jid("group@g.us"), "group@g.us");
}
