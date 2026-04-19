// Ports upstream Python commit 2a2e5c0f: any 401/403 from the Codex
// refresh-token endpoint forces ``relogin_required=True``, even when the
// JSON body lacks a recognised error code.

#include "hermes/auth/codex_oauth.hpp"

#include <gtest/gtest.h>

namespace hermes::auth {

TEST(CodexRefreshClassify, Http401ForcesReloginRegardlessOfBody) {
    auto c = classify_codex_refresh_response(401, "not even json");
    EXPECT_TRUE(c.relogin_required);
    EXPECT_FALSE(c.transient);
    EXPECT_FALSE(c.message.empty());
}

TEST(CodexRefreshClassify, Http403ForcesReloginWithEmptyBody) {
    auto c = classify_codex_refresh_response(403, "");
    EXPECT_TRUE(c.relogin_required);
    EXPECT_FALSE(c.transient);
}

TEST(CodexRefreshClassify, InvalidGrantBodyForcesRelogin) {
    // OpenAI style.  Status 400 is ambiguous on its own but the
    // body's error code is authoritative.
    auto c = classify_codex_refresh_response(
        400, R"({"error":"invalid_grant","error_description":"revoked"})");
    EXPECT_TRUE(c.relogin_required);
    EXPECT_EQ(c.error_code, "invalid_grant");
    EXPECT_EQ(c.message, "revoked");
}

TEST(CodexRefreshClassify, Http500IsTransientNotRelogin) {
    auto c = classify_codex_refresh_response(503, R"({"error":"unavailable"})");
    EXPECT_FALSE(c.relogin_required);
    EXPECT_TRUE(c.transient);
}

TEST(CodexRefreshClassify, Unknown4xxWithUnknownBodyNoRelogin) {
    // A 429 rate-limit from a different endpoint should NOT trigger
    // relogin — the token may still be valid.
    auto c = classify_codex_refresh_response(429, R"({"error":"rate_limited"})");
    EXPECT_FALSE(c.relogin_required);
    EXPECT_FALSE(c.transient);
}

TEST(CodexRefreshClassify, Http200NoReloginNoMessage) {
    auto c = classify_codex_refresh_response(
        200, R"({"access_token":"new","expires_in":3600})");
    EXPECT_FALSE(c.relogin_required);
    EXPECT_FALSE(c.transient);
    EXPECT_TRUE(c.message.empty());
}

TEST(CodexRefreshClassify, Http401WithKnownBodyStillReloginExactlyOnce) {
    // Both the status code and the body agree — result is the same.
    auto c = classify_codex_refresh_response(
        401, R"({"error":"invalid_grant"})");
    EXPECT_TRUE(c.relogin_required);
    EXPECT_EQ(c.error_code, "invalid_grant");
}

}  // namespace hermes::auth
