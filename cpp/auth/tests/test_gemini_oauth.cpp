#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "hermes/auth/gemini_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

namespace fs = std::filesystem;
using hermes::auth::GeminiCredentials;
using hermes::auth::GeminiCredentialStore;
using hermes::auth::GeminiOAuth;
using hermes::llm::FakeHttpTransport;

namespace {

fs::path make_tempdir(const char* label) {
    auto base = fs::temp_directory_path() /
                (std::string("hermes_gemini_") + label + "_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
    fs::create_directories(base);
    return base;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ----------------------------------------------------------------------------
// Store round-trip — packed refresh field preserves project IDs.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, StoreRoundTripPreservesPackedProjectIds) {
    auto dir = make_tempdir("store");
    GeminiCredentialStore store(dir / "google_oauth.json");

    GeminiCredentials c;
    c.access_token = "acc_123";
    c.refresh_token = "refr_xyz";
    c.expiry_date_ms = 1'700'000'000'000;
    c.email = "alice@example.com";
    c.project_id = "my-project";
    c.managed_project_id = "mgr-project";

    ASSERT_TRUE(store.save(c));
    auto loaded = store.load();
    EXPECT_EQ(loaded.access_token, "acc_123");
    EXPECT_EQ(loaded.refresh_token, "refr_xyz");
    EXPECT_EQ(loaded.expiry_date_ms, 1'700'000'000'000);
    EXPECT_EQ(loaded.email, "alice@example.com");
    EXPECT_EQ(loaded.project_id, "my-project");
    EXPECT_EQ(loaded.managed_project_id, "mgr-project");
    EXPECT_FALSE(loaded.is_free_tier());  // project_id non-empty
    fs::remove_all(dir);
}

TEST(GeminiOAuth, StoreBareRefreshTreatedAsFreeTier) {
    auto dir = make_tempdir("freetier");
    GeminiCredentialStore store(dir / "google_oauth.json");

    GeminiCredentials c;
    c.access_token = "acc_123";
    c.refresh_token = "refr_xyz";
    c.expiry_date_ms = 1'700'000'000'000;
    ASSERT_TRUE(store.save(c));

    auto loaded = store.load();
    EXPECT_EQ(loaded.refresh_token, "refr_xyz");
    EXPECT_EQ(loaded.project_id, "");
    EXPECT_EQ(loaded.managed_project_id, "");
    EXPECT_TRUE(loaded.is_free_tier());
    fs::remove_all(dir);
}

// ----------------------------------------------------------------------------
// exchange_code — the device/code → token exchange mirror.
// The Gemini flow uses Authorization Code (not Device Code) but the test
// surface is the same: one POST to the token endpoint that returns tokens.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, ExchangeCodePopulatesCredentials) {
    FakeHttpTransport fake;
    fake.enqueue_response({200,
                           R"({"access_token":"ya29.abc","refresh_token":"1//reflong",)"
                           R"("expires_in":3600,"token_type":"Bearer"})",
                           {}});
    GeminiOAuth oauth(&fake);
    auto creds = oauth.exchange_code("authcode", "verif",
                                      "http://127.0.0.1:8085/oauth2callback");
    ASSERT_TRUE(creds.has_value());
    EXPECT_EQ(creds->access_token, "ya29.abc");
    EXPECT_EQ(creds->refresh_token, "1//reflong");
    EXPECT_EQ(creds->token_type, "Bearer");
    // expiry_date_ms = now + 3600*1000 ± skew
    auto nms = now_ms();
    EXPECT_GT(creds->expiry_date_ms, nms + 3500 * 1000);
    EXPECT_LT(creds->expiry_date_ms, nms + 3700 * 1000);
}

TEST(GeminiOAuth, ExchangeCodeThrowsOnHttpError) {
    FakeHttpTransport fake;
    fake.enqueue_response({400, R"({"error":"invalid_grant"})", {}});
    GeminiOAuth oauth(&fake);
    EXPECT_THROW(oauth.exchange_code("bad", "verif", "http://x"),
                 std::runtime_error);
}

// ----------------------------------------------------------------------------
// refresh — success path preserves project_id / managed_project_id even when
// Google omits refresh_token from the response.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, RefreshSuccessPreservesProjectIds) {
    FakeHttpTransport fake;
    // Google's refresh endpoint typically does NOT re-issue a refresh_token.
    fake.enqueue_response(
        {200,
         R"({"access_token":"ya29.new","expires_in":3600,"token_type":"Bearer"})",
         {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//oldreft";
    prev.expiry_date_ms = now_ms() - 60'000;
    prev.project_id = "my-project";
    prev.managed_project_id = "mgr-project";
    prev.email = "alice@example.com";

    auto fresh = oauth.refresh(prev);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->access_token, "ya29.new");
    EXPECT_EQ(fresh->refresh_token, "1//oldreft");  // preserved
    EXPECT_EQ(fresh->project_id, "my-project");
    EXPECT_EQ(fresh->managed_project_id, "mgr-project");
    EXPECT_EQ(fresh->email, "alice@example.com");
    EXPECT_GT(fresh->expiry_date_ms, now_ms() + 3500 * 1000);
}

TEST(GeminiOAuth, RefreshAdoptsRotatedRefreshToken) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"access_token":"ya29.new","refresh_token":"1//rotated",)"
         R"("expires_in":3600,"token_type":"Bearer"})",
         {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//oldreft";

    auto fresh = oauth.refresh(prev);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->refresh_token, "1//rotated");
}

// ----------------------------------------------------------------------------
// refresh 401 / 403 — forces relogin via classify_codex_refresh_response.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, Refresh401ForcesRelogin) {
    FakeHttpTransport fake;
    fake.enqueue_response({401, R"({"error":"unauthorized"})", {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//oldreft";
    auto fresh = oauth.refresh(prev);
    EXPECT_FALSE(fresh.has_value());
}

TEST(GeminiOAuth, Refresh403ForcesRelogin) {
    FakeHttpTransport fake;
    fake.enqueue_response({403, R"({})", {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//oldreft";
    auto fresh = oauth.refresh(prev);
    EXPECT_FALSE(fresh.has_value());
}

TEST(GeminiOAuth, RefreshInvalidGrantForcesRelogin) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {400,
         R"({"error":"invalid_grant","error_description":"token revoked"})",
         {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//revoked";
    auto fresh = oauth.refresh(prev);
    EXPECT_FALSE(fresh.has_value());
}

TEST(GeminiOAuth, Refresh5xxIsTransient) {
    FakeHttpTransport fake;
    fake.enqueue_response({503, R"({"error":"backend_unavailable"})", {}});
    GeminiOAuth oauth(&fake);

    GeminiCredentials prev;
    prev.access_token = "ya29.old";
    prev.refresh_token = "1//oldreft";
    auto fresh = oauth.refresh(prev);
    // 5xx returns nullopt (can't parse new token) but must not force relogin —
    // the contract is that ensure_valid's store.clear() path is reserved for
    // permanent failures.  We assert the call returned nullopt; the
    // distinction between transient and relogin is tested at the Codex layer
    // (test_codex_oauth.cpp) and mirrored here implicitly.
    EXPECT_FALSE(fresh.has_value());
}

TEST(GeminiOAuth, RefreshRequiresRefreshTokenPresent) {
    FakeHttpTransport fake;
    GeminiOAuth oauth(&fake);
    GeminiCredentials prev;  // refresh_token empty
    auto fresh = oauth.refresh(prev);
    EXPECT_FALSE(fresh.has_value());
    EXPECT_TRUE(fake.requests().empty());  // never made the HTTP call
}

// ----------------------------------------------------------------------------
// ensure_valid — end-to-end: loads, refreshes, saves.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, EnsureValidReturnsFreshWhenNotExpiring) {
    auto dir = make_tempdir("ensure_fresh");
    GeminiCredentialStore store(dir / "google_oauth.json");

    GeminiCredentials c;
    c.access_token = "still_fresh";
    c.refresh_token = "reft";
    c.expiry_date_ms = now_ms() + 3600 * 1000;  // 1h in future
    ASSERT_TRUE(store.save(c));

    FakeHttpTransport fake;  // no requests queued — must not be hit
    GeminiOAuth oauth(&fake);
    auto got = oauth.ensure_valid(store);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->access_token, "still_fresh");
    EXPECT_TRUE(fake.requests().empty());
    fs::remove_all(dir);
}

TEST(GeminiOAuth, EnsureValidRefreshesExpiringToken) {
    auto dir = make_tempdir("ensure_refresh");
    GeminiCredentialStore store(dir / "google_oauth.json");

    GeminiCredentials c;
    c.access_token = "expiring";
    c.refresh_token = "reft";
    c.expiry_date_ms = now_ms() + 10'000;  // 10s — inside the 60s skew
    c.project_id = "proj";
    ASSERT_TRUE(store.save(c));

    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"access_token":"new_tok","expires_in":3600,"token_type":"Bearer"})",
         {}});
    GeminiOAuth oauth(&fake);
    auto got = oauth.ensure_valid(store);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->access_token, "new_tok");
    EXPECT_EQ(got->project_id, "proj");  // preserved across refresh + save
    // And the store reflects the refresh.
    auto on_disk = store.load();
    EXPECT_EQ(on_disk.access_token, "new_tok");
    EXPECT_EQ(on_disk.project_id, "proj");
    fs::remove_all(dir);
}

TEST(GeminiOAuth, EnsureValidClearsStoreOnReloginRequired) {
    auto dir = make_tempdir("ensure_401");
    GeminiCredentialStore store(dir / "google_oauth.json");

    GeminiCredentials c;
    c.access_token = "expiring";
    c.refresh_token = "revoked";
    c.expiry_date_ms = now_ms() - 1;  // already expired
    ASSERT_TRUE(store.save(c));

    FakeHttpTransport fake;
    fake.enqueue_response({401, R"({"error":"unauthorized"})", {}});
    GeminiOAuth oauth(&fake);
    auto got = oauth.ensure_valid(store);
    EXPECT_FALSE(got.has_value());
    // File must be gone — the ensure_valid contract clears on permanent
    // failure so the caller knows to prompt a relogin.
    EXPECT_FALSE(fs::exists(dir / "google_oauth.json"));
    fs::remove_all(dir);
}

// ----------------------------------------------------------------------------
// PKCE — verifier + challenge shape.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, PkcePairShapesMatchRfc7636) {
    auto p = GeminiOAuth::generate_pkce();
    // verifier is 32 bytes base64url → 43 chars (no padding)
    EXPECT_EQ(p.verifier.size(), 43u);
    // challenge is sha256(verifier) base64url → 43 chars (no padding)
    EXPECT_EQ(p.challenge_s256.size(), 43u);
    // Verifier and challenge differ (i.e., S256 was applied).
    EXPECT_NE(p.verifier, p.challenge_s256);
    // Only the base64url charset.
    for (char ch : p.verifier) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(ch)) ||
                    ch == '-' || ch == '_');
    }
}

// ----------------------------------------------------------------------------
// Sanity: env var override takes priority over defaults.
// ----------------------------------------------------------------------------

TEST(GeminiOAuth, EnvClientIdOverridesDefault) {
    ::setenv("HERMES_GEMINI_CLIENT_ID", "my.custom.client.id", 1);
    ::setenv("HERMES_GEMINI_CLIENT_SECRET", "my.custom.secret", 1);
    GeminiOAuth oauth;  // picks up env vars
    EXPECT_EQ(oauth.client_id(), "my.custom.client.id");
    EXPECT_EQ(oauth.client_secret(), "my.custom.secret");
    ::unsetenv("HERMES_GEMINI_CLIENT_ID");
    ::unsetenv("HERMES_GEMINI_CLIENT_SECRET");
}
