// Phase 12 — Matrix E2EE (libolm) wrapper tests.
//
// Most tests require libolm to be available at build time.  They use
// GTEST_SKIP() when !available() so the suite still passes on hosts
// without libolm-dev installed.  The "no-op when unavailable" test is
// always run, verifying that the fallback API contract holds.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "../platforms/olm_session.hpp"

using hermes::gateway::platforms::MegolmInboundSession;
using hermes::gateway::platforms::MegolmOutboundSession;
using hermes::gateway::platforms::OlmAccount;
using hermes::gateway::platforms::OlmSession;

namespace {

#define REQUIRE_OLM()                                                       \
    do {                                                                    \
        OlmAccount _probe;                                                  \
        if (!_probe.available()) GTEST_SKIP() << "libolm not available";    \
    } while (0)

}  // namespace

TEST(MatrixE2EE, AccountCreationHasIdentityKeys) {
    REQUIRE_OLM();
    OlmAccount a;
    ASSERT_TRUE(a.available());
    auto keys = a.identity_keys_json();
    EXPECT_FALSE(keys.empty());
    auto parsed = nlohmann::json::parse(keys);
    EXPECT_TRUE(parsed.contains("curve25519"));
    EXPECT_TRUE(parsed.contains("ed25519"));
}

TEST(MatrixE2EE, AccountPickleRoundTripPreservesIdentity) {
    REQUIRE_OLM();
    OlmAccount a;
    ASSERT_TRUE(a.available());
    auto keys_before = a.identity_keys_json();
    auto pickled = a.pickle("secret");
    ASSERT_FALSE(pickled.empty());

    OlmAccount b;
    ASSERT_TRUE(b.unpickle(pickled, "secret"));
    auto keys_after = b.identity_keys_json();
    // The JSON objects encode the same keys even if whitespace differs.
    EXPECT_EQ(nlohmann::json::parse(keys_before),
              nlohmann::json::parse(keys_after));
}

TEST(MatrixE2EE, OneTimeKeysGenerateAndList) {
    REQUIRE_OLM();
    OlmAccount a;
    ASSERT_TRUE(a.available());
    ASSERT_TRUE(a.generate_one_time_keys(5));
    auto otks_json = a.one_time_keys_json();
    ASSERT_FALSE(otks_json.empty());
    auto parsed = nlohmann::json::parse(otks_json);
    ASSERT_TRUE(parsed.contains("curve25519"));
    EXPECT_EQ(parsed["curve25519"].size(), 5u);
}

TEST(MatrixE2EE, OutboundSessionCreateAndEncrypt) {
    REQUIRE_OLM();
    OlmAccount alice;
    OlmAccount bob;
    ASSERT_TRUE(bob.generate_one_time_keys(1));

    auto bob_idkeys = nlohmann::json::parse(bob.identity_keys_json());
    auto bob_curve = bob_idkeys["curve25519"].get<std::string>();
    auto bob_otks = nlohmann::json::parse(bob.one_time_keys_json());
    ASSERT_TRUE(bob_otks.contains("curve25519"));
    std::string bob_otk;
    for (auto& [k, v] : bob_otks["curve25519"].items()) {
        bob_otk = v.get<std::string>();
        break;
    }
    ASSERT_FALSE(bob_otk.empty());

    auto session = OlmSession::create_outbound(alice, bob_curve, bob_otk);
    ASSERT_TRUE(session.has_value());
    auto ct = session->encrypt("hello bob");
    EXPECT_FALSE(ct.empty());
    auto parsed = nlohmann::json::parse(ct);
    EXPECT_TRUE(parsed.contains("type"));
    EXPECT_TRUE(parsed.contains("body"));
}

TEST(MatrixE2EE, OlmRoundTripTwoAccounts) {
    REQUIRE_OLM();
    OlmAccount alice;
    OlmAccount bob;
    ASSERT_TRUE(bob.generate_one_time_keys(1));

    auto alice_idkeys = nlohmann::json::parse(alice.identity_keys_json());
    auto alice_curve = alice_idkeys["curve25519"].get<std::string>();
    auto bob_idkeys = nlohmann::json::parse(bob.identity_keys_json());
    auto bob_curve = bob_idkeys["curve25519"].get<std::string>();
    auto bob_otks = nlohmann::json::parse(bob.one_time_keys_json());
    std::string bob_otk;
    for (auto& [k, v] : bob_otks["curve25519"].items()) {
        bob_otk = v.get<std::string>();
        break;
    }

    auto alice_session = OlmSession::create_outbound(alice, bob_curve, bob_otk);
    ASSERT_TRUE(alice_session.has_value());

    const std::string plaintext = "secret handshake";
    auto ct_json = alice_session->encrypt(plaintext);
    auto parsed = nlohmann::json::parse(ct_json);
    int type = parsed["type"].get<int>();
    std::string body = parsed["body"].get<std::string>();
    EXPECT_EQ(type, 0);  // first message is a pre-key message

    auto bob_session = OlmSession::create_inbound(bob, alice_curve, body);
    ASSERT_TRUE(bob_session.has_value());
    auto decrypted = bob_session->decrypt(type, body);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(*decrypted, plaintext);
}

TEST(MatrixE2EE, MegolmOutboundSessionIdAndKey) {
    REQUIRE_OLM();
    MegolmOutboundSession out;
    ASSERT_TRUE(out.available());
    auto sid = out.session_id();
    auto skey = out.session_key();
    EXPECT_FALSE(sid.empty());
    EXPECT_FALSE(skey.empty());
    EXPECT_EQ(out.message_index(), 0u);
}

TEST(MatrixE2EE, MegolmRoundTrip) {
    REQUIRE_OLM();
    MegolmOutboundSession out;
    ASSERT_TRUE(out.available());
    auto session_key = out.session_key();

    MegolmInboundSession in;
    ASSERT_TRUE(in.init_from_session_key(session_key));
    EXPECT_EQ(in.session_id(), out.session_id());

    const std::string plaintext = "hello room";
    auto ct = out.encrypt(plaintext);
    ASSERT_FALSE(ct.empty());

    auto decrypted = in.decrypt(ct);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(*decrypted, plaintext);
}

TEST(MatrixE2EE, MegolmOutboundPickleRoundTrip) {
    REQUIRE_OLM();
    MegolmOutboundSession out;
    ASSERT_TRUE(out.available());
    auto sid_before = out.session_id();
    auto pickled = out.pickle("pw");
    ASSERT_FALSE(pickled.empty());

    MegolmOutboundSession restored;
    ASSERT_TRUE(restored.unpickle(pickled, "pw"));
    EXPECT_EQ(restored.session_id(), sid_before);
}

// Always-runs: fallback contract when libolm is missing.  When present, we
// simply assert that the constructor path yields an available() object —
// that is, the wrapper's contract holds in both directions.
TEST(MatrixE2EE, FallbackContractHolds) {
    OlmAccount a;
    if (!a.available()) {
        // Compile-time opt-out path.
        EXPECT_TRUE(a.identity_keys_json().empty());
        EXPECT_TRUE(a.pickle("x").empty());
        EXPECT_FALSE(a.generate_one_time_keys(1));

        OlmSession s;
        EXPECT_FALSE(s.available());
        EXPECT_TRUE(s.encrypt("hello").empty());
        EXPECT_FALSE(s.decrypt(0, "body").has_value());

        MegolmOutboundSession m;
        EXPECT_FALSE(m.available());
        EXPECT_TRUE(m.encrypt("hi").empty());

        MegolmInboundSession mi;
        EXPECT_FALSE(mi.available());
        EXPECT_FALSE(mi.decrypt("x").has_value());
    } else {
        EXPECT_FALSE(a.identity_keys_json().empty());
    }
}
