#include <gtest/gtest.h>

#include "hermes/auth/mcp_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

}  // namespace
