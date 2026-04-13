// Phase 12.6 — Signal encryption / safety-number / expiration tests.
//
// signal-cli implements the Signal protocol natively, so "encryption"
// testing here means validating our wire-level use of the REST API:
//  * messages are sent encrypted by default (the REST bridge always
//    encrypts — we just check we hit the /v2/send endpoint);
//  * disappearing-message timers are propagated;
//  * group identifiers are handled;
//  * safety-number verification reads /v1/identities correctly.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "../platforms/signal.hpp"

#include <hermes/llm/llm_client.hpp>

using hermes::gateway::platforms::SignalAdapter;
using hermes::llm::FakeHttpTransport;

namespace {
SignalAdapter::Config cfg() {
    SignalAdapter::Config c;
    c.http_url = "http://127.0.0.1:8080";
    c.account = "+15551234567";
    return c;
}
}  // namespace

TEST(SignalEncryption, NormalizeIdentifiers) {
    EXPECT_EQ(SignalAdapter::normalize_identifier("+12125551234"),
              "+12125551234");
    EXPECT_EQ(SignalAdapter::normalize_identifier("12125551234"),
              "+12125551234");
    EXPECT_EQ(SignalAdapter::normalize_identifier("ABCDEF12-3456-7890-ABCD-EF1234567890"),
              "abcdef12-3456-7890-abcd-ef1234567890");
    EXPECT_EQ(SignalAdapter::normalize_identifier("@group.abc="),
              "group.abc=");
    EXPECT_EQ(SignalAdapter::normalize_identifier("group:abc="),
              "group.abc=");
    EXPECT_EQ(SignalAdapter::normalize_identifier("group.abc="),
              "group.abc=");
    EXPECT_EQ(SignalAdapter::normalize_identifier(""), "");
}

TEST(SignalEncryption, IsGroupIdentifier) {
    EXPECT_TRUE(SignalAdapter::is_group_identifier("group.abc="));
    EXPECT_TRUE(SignalAdapter::is_group_identifier("@group.abc="));
    EXPECT_TRUE(SignalAdapter::is_group_identifier("group:abc="));
    EXPECT_FALSE(SignalAdapter::is_group_identifier("+12125551234"));
    EXPECT_FALSE(SignalAdapter::is_group_identifier(
        "abcdef12-3456-7890-abcd-ef1234567890"));
}

TEST(SignalEncryption, SendDirectRecipientsHitsV2Send) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"timestamp":"1"})", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.send("+12125551234", "hello"));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/v2/send"), std::string::npos);

    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("recipients"));
    EXPECT_EQ(body["recipients"][0].get<std::string>(), "+12125551234");
    EXPECT_FALSE(body.contains("group-id"));
}

TEST(SignalEncryption, SendGroupUsesGroupIdField) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"timestamp":"1"})", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.send("group.AbCd=", "hi team"));

    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("group-id"));
    EXPECT_EQ(body["group-id"].get<std::string>(), "AbCd=");
    EXPECT_FALSE(body.contains("recipients"));
}

TEST(SignalEncryption, SendWithExpirationAddsField) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"timestamp":"1"})", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.send_with_expiration("+12125551234", "hi", 3600));

    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("expires_in_seconds"));
    EXPECT_EQ(body["expires_in_seconds"].get<int>(), 3600);
}

TEST(SignalEncryption, SendHonoursRememberedExpiration) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"timestamp":"1"})", {}});

    SignalAdapter adapter(cfg(), &fake);
    adapter.remember_expiration("+12125551234", 86400);
    EXPECT_EQ(adapter.expiration_for("+12125551234"), 86400);

    EXPECT_TRUE(adapter.send("+12125551234", "hi"));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["expires_in_seconds"].get<int>(), 86400);
}

TEST(SignalEncryption, RememberExpirationZeroClears) {
    SignalAdapter adapter(cfg());
    adapter.remember_expiration("+1", 60);
    EXPECT_EQ(adapter.expiration_for("+1"), 60);
    adapter.remember_expiration("+1", 0);
    EXPECT_EQ(adapter.expiration_for("+1"), 0);
}

TEST(SignalEncryption, SetExpirationPostsToEndpoint) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "{}", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.set_expiration("+12125551234", 86400));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/v1/expiration/"),
              std::string::npos);
    EXPECT_NE(fake.requests()[0].url.find("+15551234567/"),
              std::string::npos);
    // After set, it should be remembered.
    EXPECT_EQ(adapter.expiration_for("+12125551234"), 86400);
}

TEST(SignalEncryption, SetExpirationFailurePropagates) {
    FakeHttpTransport fake;
    fake.enqueue_response({500, "err", {}});
    SignalAdapter adapter(cfg(), &fake);
    EXPECT_FALSE(adapter.set_expiration("+12125551234", 86400));
    EXPECT_EQ(adapter.expiration_for("+12125551234"), 0);
}

TEST(SignalEncryption, FetchSafetyNumberParsesEntry) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"([
        {"number":"+12125551234","uuid":"abc","safety_number":"123456 789012",
         "trust_level":"VERIFIED"},
        {"number":"+13335557777","safety_number":"aa bb","trust_level":"UNTRUSTED"}
    ])", {}});

    SignalAdapter adapter(cfg(), &fake);
    auto sn = adapter.fetch_safety_number("+12125551234");
    ASSERT_TRUE(sn.has_value());
    EXPECT_EQ(sn->recipient, "+12125551234");
    EXPECT_EQ(sn->fingerprint, "123456 789012");
    EXPECT_EQ(sn->trust, SignalAdapter::TrustLevel::Verified);
}

TEST(SignalEncryption, VerifySafetyNumberTrue) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"([
        {"number":"+12125551234","safety_number":"x","trust_level":"VERIFIED"}
    ])", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.verify_safety_number("+12125551234"));
}

TEST(SignalEncryption, VerifySafetyNumberFalseForUntrusted) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"([
        {"number":"+12125551234","safety_number":"x","trust_level":"UNTRUSTED"}
    ])", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_FALSE(adapter.verify_safety_number("+12125551234"));
}

TEST(SignalEncryption, VerifySafetyNumberFalseForUnknown) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"([])", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_FALSE(adapter.verify_safety_number("+12125551234"));
}

TEST(SignalEncryption, VerifySafetyNumberCamelCaseTrustLevel) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"([
        {"number":"+12125551234","fingerprint":"x","trustLevel":"VERIFIED"}
    ])", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.verify_safety_number("+12125551234"));
}

TEST(SignalEncryption, TrustIdentityPostsCorrectPayload) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "{}", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.trust_identity("+12125551234", /*verified=*/true));

    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("/trust/+12125551234"),
              std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_TRUE(body["verified_safety_number"].get<bool>());
    EXPECT_FALSE(body["trust_all_known_keys"].get<bool>());
}

TEST(SignalEncryption, SendingPhonePassesThrough) {
    // Regression: raw digits without + should be promoted to E.164.
    FakeHttpTransport fake;
    fake.enqueue_response({200, "{}", {}});

    SignalAdapter adapter(cfg(), &fake);
    EXPECT_TRUE(adapter.send("12125551234", "hi"));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["recipients"][0].get<std::string>(), "+12125551234");
}
