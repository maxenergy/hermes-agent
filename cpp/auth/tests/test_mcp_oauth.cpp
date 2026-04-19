#include <gtest/gtest.h>

#include "hermes/auth/mcp_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using hermes::auth::McpOAuth;
using hermes::auth::McpOAuthToken;
using hermes::llm::FakeHttpTransport;

namespace {

class McpOAuthTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_home;

    void SetUp() override {
        tmp_home = std::filesystem::temp_directory_path() /
                   ("hermes_mcp_oauth_" + std::to_string(::getpid()));
        std::filesystem::remove_all(tmp_home);
        std::filesystem::create_directories(tmp_home);
        ::setenv("HERMES_HOME", tmp_home.c_str(), 1);
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp_home);
        ::unsetenv("HERMES_HOME");
    }
};

TEST_F(McpOAuthTest, PkceChallengeIsBase64UrlAndStable) {
    auto pkce = McpOAuth::make_pkce();
    EXPECT_FALSE(pkce.verifier.empty());
    EXPECT_FALSE(pkce.challenge.empty());
    EXPECT_EQ(pkce.method, "S256");
    // base64url: no '+' / '/' / '='
    for (char c : pkce.challenge) {
        EXPECT_TRUE(c != '+' && c != '/' && c != '=');
    }
}

TEST_F(McpOAuthTest, ExtractPortHandlesTypicalRedirect) {
    EXPECT_EQ(McpOAuth::extract_port("http://127.0.0.1:54321/callback"), 54321);
    EXPECT_EQ(McpOAuth::extract_port("http://localhost:7/x"), 7);
    EXPECT_EQ(McpOAuth::extract_port("http://localhost/"), 0);
    EXPECT_EQ(McpOAuth::extract_port("not-a-url"), 0);
}

TEST_F(McpOAuthTest, BuildAuthorizeUrlIncludesPkceParams) {
    hermes::auth::McpOAuthConfig cfg;
    cfg.server_url = "https://mcp.example.com";
    cfg.client_id = "abc";
    cfg.redirect_uri = "http://127.0.0.1:9999/callback";
    cfg.scopes = {"read", "write"};
    auto pkce = McpOAuth::make_pkce();
    auto url = McpOAuth::build_authorize_url(cfg, pkce, "st4te");
    EXPECT_NE(url.find("response_type=code"), std::string::npos);
    EXPECT_NE(url.find("client_id=abc"), std::string::npos);
    EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(url.find("state=st4te"), std::string::npos);
    EXPECT_NE(url.find("scope=read%20write"), std::string::npos);
}

TEST_F(McpOAuthTest, SaveAndLoadTokenRoundTrip) {
    McpOAuthToken tok;
    tok.access_token = "a1b2";
    tok.refresh_token = "r9x8";
    tok.expiry_date_ms = 1700000000000;
    tok.scope = "read";
    EXPECT_TRUE(McpOAuth::save_token("my_server", tok));

    auto loaded = McpOAuth::load_token("my_server");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->access_token, "a1b2");
    EXPECT_EQ(loaded->refresh_token, "r9x8");
    EXPECT_EQ(loaded->scope, "read");
}

TEST_F(McpOAuthTest, LoadTokenReturnsNulloptWhenMissing) {
    auto loaded = McpOAuth::load_token("no_such_server");
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(McpOAuthTest, RefreshWithoutTokenReturnsNullopt) {
    FakeHttpTransport fake;
    McpOAuth oauth(&fake);
    hermes::auth::McpOAuthConfig cfg;
    cfg.server_url = "https://mcp.example.com";
    cfg.client_id = "c";
    McpOAuthToken tok;  // no refresh_token
    EXPECT_FALSE(oauth.refresh(cfg, tok).has_value());
}

TEST_F(McpOAuthTest, RefreshParsesSuccessfulResponse) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"access_token":"new_at","expires_in":3600,"token_type":"Bearer"})",
         {}});
    McpOAuth oauth(&fake);
    hermes::auth::McpOAuthConfig cfg;
    cfg.server_url = "https://mcp.example.com";
    cfg.token_url = "https://mcp.example.com/token";
    cfg.client_id = "c";
    McpOAuthToken cur;
    cur.refresh_token = "rt";
    auto res = oauth.refresh(cfg, cur);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->access_token, "new_at");
    EXPECT_EQ(res->refresh_token, "rt");  // preserved
    EXPECT_GT(res->expiry_date_ms, 0);
}

// ---------------------------------------------------------------------------
// MCPOAuthManager — mtime watch + dedup'd recovery + reconnect signalling.
// ---------------------------------------------------------------------------

using hermes::auth::McpOAuthConfig;
using hermes::auth::MCPOAuthManager;
using hermes::auth::McpOAuthRecovery;

namespace {

McpOAuthToken token_with(const std::string& at, const std::string& rt = "rt") {
    McpOAuthToken t;
    t.access_token = at;
    t.refresh_token = rt;
    t.expiry_date_ms = 1'700'000'000'000;
    t.token_type = "Bearer";
    return t;
}

McpOAuthConfig cfg_for(const std::string& name) {
    McpOAuthConfig cfg;
    cfg.server_url = "https://" + name + ".example.com";
    cfg.token_url = "https://" + name + ".example.com/token";
    cfg.client_id = "cid-" + name;
    return cfg;
}

}  // namespace

TEST_F(McpOAuthTest, ManagerGetTokenReturnsNulloptWhenMissing) {
    MCPOAuthManager mgr;
    EXPECT_FALSE(mgr.get_token("no_such_server").has_value());
}

TEST_F(McpOAuthTest, ManagerGetTokenLoadsFromDiskOnFirstCall) {
    McpOAuth::save_token("srv_a", token_with("disk_at"));
    MCPOAuthManager mgr;
    auto tok = mgr.get_token("srv_a");
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->access_token, "disk_at");
}

TEST_F(McpOAuthTest, ManagerMtimeWatchRefreshesCacheAndFiresReconnect) {
    McpOAuth::save_token("srv_b", token_with("initial_at"));
    MCPOAuthManager mgr;

    std::atomic<int> fires{0};
    mgr.subscribe_reconnect("srv_b", [&](const std::string& s) {
        EXPECT_EQ(s, "srv_b");
        fires.fetch_add(1);
    });

    // First read — caches the token, no reconnect fire (this is the initial
    // load, not an external refresh).
    auto first = mgr.get_token("srv_b");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->access_token, "initial_at");
    EXPECT_EQ(fires.load(), 0);

    // Sleep to ensure mtime granularity ticks over on file systems with a
    // coarse last_write_time resolution.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // External "refresh" — another process wrote a new token file.
    McpOAuth::save_token("srv_b", token_with("external_at"));

    auto second = mgr.get_token("srv_b");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->access_token, "external_at");
    EXPECT_EQ(fires.load(), 1);
}

TEST_F(McpOAuthTest, ManagerSubscribeUnsubscribe) {
    MCPOAuthManager mgr;
    auto h1 = mgr.subscribe_reconnect("srv_c", [](const std::string&) {});
    auto h2 = mgr.subscribe_reconnect("srv_c", [](const std::string&) {});
    EXPECT_EQ(mgr.subscriber_count("srv_c"), 2u);
    mgr.unsubscribe_reconnect("srv_c", h1);
    EXPECT_EQ(mgr.subscriber_count("srv_c"), 1u);
    mgr.unsubscribe_reconnect("srv_c", h2);
    EXPECT_EQ(mgr.subscriber_count("srv_c"), 0u);
}

TEST_F(McpOAuthTest, ManagerHandle401NoCredentials) {
    MCPOAuthManager mgr;
    auto rc = mgr.handle_401("srv_none", cfg_for("srv_none"));
    EXPECT_EQ(rc, McpOAuthRecovery::kNoCredentials);
}

TEST_F(McpOAuthTest, ManagerHandle401PicksUpExternalRefresh) {
    // Cache holds stale token; disk has a newer one (mtime advanced).
    McpOAuth::save_token("srv_d", token_with("stale_at"));
    MCPOAuthManager mgr;
    auto cur = mgr.get_token("srv_d");  // primes cache
    ASSERT_TRUE(cur.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    McpOAuth::save_token("srv_d", token_with("fresh_at"));

    std::atomic<int> fires{0};
    mgr.subscribe_reconnect("srv_d", [&](const std::string&) { ++fires; });

    FakeHttpTransport fake;  // no queued responses — must not be hit
    MCPOAuthManager mgr2(&fake);  // separate so our fake is what gets used
    // Re-prime mgr2 to track mtime
    auto seed = mgr2.get_token("srv_d");
    ASSERT_TRUE(seed.has_value());
    EXPECT_EQ(seed->access_token, "fresh_at");
    std::atomic<int> fires2{0};
    mgr2.subscribe_reconnect("srv_d", [&](const std::string&) { ++fires2; });

    // Now another external write with an even-newer token, simulating a
    // cron-driven refresh mid-401.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    McpOAuth::save_token("srv_d", token_with("newer_at"));

    auto rc = mgr2.handle_401("srv_d", cfg_for("srv_d"));
    EXPECT_EQ(rc, McpOAuthRecovery::kRefreshedOnDisk);
    EXPECT_EQ(fires2.load(), 1);
    EXPECT_TRUE(fake.requests().empty());  // disk recovery, no network call
}

TEST_F(McpOAuthTest, ManagerHandle401InPlaceRefreshSuccess) {
    // Cache and disk have the same token; the manager must call the token
    // endpoint.
    McpOAuth::save_token("srv_e", token_with("expiring_at", "refr"));

    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"access_token":"refreshed_at","expires_in":3600,"token_type":"Bearer"})",
         {}});
    MCPOAuthManager mgr(&fake);
    auto seed = mgr.get_token("srv_e");
    ASSERT_TRUE(seed.has_value());

    std::atomic<int> fires{0};
    mgr.subscribe_reconnect("srv_e", [&](const std::string&) { ++fires; });

    auto rc = mgr.handle_401("srv_e", cfg_for("srv_e"));
    EXPECT_EQ(rc, McpOAuthRecovery::kRefreshedInPlace);
    EXPECT_EQ(fires.load(), 1);
    // Disk + cache updated.
    auto after = mgr.peek_cached("srv_e");
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->access_token, "refreshed_at");
    auto on_disk = McpOAuth::load_token("srv_e");
    ASSERT_TRUE(on_disk.has_value());
    EXPECT_EQ(on_disk->access_token, "refreshed_at");
}

TEST_F(McpOAuthTest, ManagerHandle401NeedsReloginWhenRefreshFails) {
    McpOAuth::save_token("srv_f", token_with("revoked_at", "revoked_rt"));
    FakeHttpTransport fake;
    fake.enqueue_response({401, R"({"error":"invalid_grant"})", {}});
    MCPOAuthManager mgr(&fake);
    (void)mgr.get_token("srv_f");

    auto rc = mgr.handle_401("srv_f", cfg_for("srv_f"));
    EXPECT_EQ(rc, McpOAuthRecovery::kNeedsRelogin);
}

TEST_F(McpOAuthTest, ManagerHandle401DedupsConcurrentCallers) {
    // Two threads hitting handle_401 for the same server — the manager
    // must serialize them so only one refresh HTTP call fires.
    McpOAuth::save_token("srv_g", token_with("concurrent_at", "rt"));
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200,
         R"({"access_token":"winner_at","expires_in":3600,"token_type":"Bearer"})",
         {}});
    // If the second thread also hit the network, it would return ""/discard
    // since we only queued one response — the fake would return a 0-status
    // empty-body response, causing the second handle_401 to report
    // kNeedsRelogin.  By queuing only one response we assert the second
    // caller takes the "disk changed" fast path.
    MCPOAuthManager mgr(&fake);
    (void)mgr.get_token("srv_g");  // prime mtime

    std::atomic<int> fires{0};
    mgr.subscribe_reconnect("srv_g", [&](const std::string&) { ++fires; });

    std::atomic<int> refreshed_count{0};
    std::atomic<int> disk_count{0};
    std::atomic<int> other_count{0};

    auto worker = [&]() {
        auto rc = mgr.handle_401("srv_g", cfg_for("srv_g"));
        if (rc == McpOAuthRecovery::kRefreshedInPlace) ++refreshed_count;
        else if (rc == McpOAuthRecovery::kRefreshedOnDisk) ++disk_count;
        else ++other_count;
    };

    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();

    // Exactly one thread performed the network refresh, the other rode
    // on the updated cache via the disk-mtime path.
    EXPECT_EQ(refreshed_count.load(), 1);
    EXPECT_EQ(disk_count.load(), 1);
    EXPECT_EQ(other_count.load(), 0);
    // Reconnect fired at least once (ordering between the two paths is
    // not guaranteed — in-place fires once, disk reload fires once).
    EXPECT_GE(fires.load(), 1);
    // Only the one queued HTTP call was actually consumed.
    EXPECT_EQ(fake.requests().size(), 1u);
}

TEST_F(McpOAuthTest, ManagerInvalidateDropsCacheButKeepsDisk) {
    McpOAuth::save_token("srv_h", token_with("keep_at"));
    MCPOAuthManager mgr;
    (void)mgr.get_token("srv_h");
    EXPECT_TRUE(mgr.peek_cached("srv_h").has_value());
    mgr.invalidate("srv_h");
    EXPECT_FALSE(mgr.peek_cached("srv_h").has_value());
    // Disk untouched — another manager instance can still load it.
    MCPOAuthManager mgr2;
    auto tok = mgr2.get_token("srv_h");
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->access_token, "keep_at");
}

}  // namespace
