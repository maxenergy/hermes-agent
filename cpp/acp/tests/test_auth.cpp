// ACP authentication tests.
#include "hermes/acp/acp_adapter.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::acp {
namespace {

AcpConfig empty_env_config() {
    AcpConfig c;
    // Force the adapter to ignore any real ANTHROPIC/OPENAI env vars in the
    // test process, so tests have deterministic behavior.
    c.forced_env_token = "";
    return c;
}

TEST(AcpAuthTest, AuthMethodsAdvertised) {
    AcpAdapter adapter(empty_env_config());
    auto methods = adapter.auth_methods();
    ASSERT_TRUE(methods.is_array());
    ASSERT_GE(methods.size(), 2u);
    std::string found;
    for (const auto& m : methods) {
        found += m.value("id", "") + ",";
    }
    EXPECT_NE(found.find("api-key"), std::string::npos);
    EXPECT_NE(found.find("oauth"), std::string::npos);
}

TEST(AcpAuthTest, UnsupportedMethodRejected) {
    AcpAdapter adapter(empty_env_config());
    auto r = adapter.authenticate("telepathy", nlohmann::json::object());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("unsupported"), std::string::npos);
}

TEST(AcpAuthTest, ApiKeyAuthRequiresCredential) {
    AcpAdapter adapter(empty_env_config());
    // No api_key param, no env token -> fail.
    auto r = adapter.authenticate("api-key", nlohmann::json::object());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("missing"), std::string::npos);
}

TEST(AcpAuthTest, ApiKeyAuthWithParamSucceeds) {
    AcpAdapter adapter(empty_env_config());
    nlohmann::json params = {{"api_key", "sk-test-xyz"}};
    auto r = adapter.authenticate("api-key", params);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(r.session_id.empty());
    EXPECT_EQ(r.method_id, "api-key");
    // Session token now grants access.
    EXPECT_TRUE(adapter.is_authenticated(r.session_id));
    EXPECT_FALSE(adapter.is_authenticated("does-not-exist"));
}

TEST(AcpAuthTest, OAuthRequiresAccessToken) {
    AcpAdapter adapter(empty_env_config());
    auto r = adapter.authenticate("oauth", nlohmann::json::object());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("missing"), std::string::npos);

    nlohmann::json params = {{"access_token", "oauth-token-abc"}};
    auto r2 = adapter.authenticate("oauth", params);
    EXPECT_TRUE(r2.ok);
    EXPECT_TRUE(adapter.is_authenticated(r2.session_id));
}

TEST(AcpAuthTest, EnvTokenGrantsImplicitAuth) {
    AcpConfig cfg;
    cfg.forced_env_token = "env-token-value";
    AcpAdapter adapter(cfg);
    EXPECT_TRUE(adapter.has_env_credential());
    // Empty session id => implicit auth via env token.
    EXPECT_TRUE(adapter.is_authenticated(""));
    // Unknown session id still not valid (without passing through authenticate).
    EXPECT_FALSE(adapter.is_authenticated("bogus"));
}

TEST(AcpAuthTest, UnauthenticatedRpcBlocked) {
    AcpAdapter adapter(empty_env_config());
    nlohmann::json req = {{"method", "new_session"}};
    auto resp = adapter.handle_request(req);
    EXPECT_EQ(resp["status"], "error");
    EXPECT_EQ(resp["error"], "unauthenticated");
}

TEST(AcpAuthTest, AuthenticateRpcThenDispatch) {
    AcpAdapter adapter(empty_env_config());

    // 1. Authenticate via RPC.
    nlohmann::json auth_req = {
        {"method", "authenticate"},
        {"method_id", "api-key"},
        {"params", {{"api_key", "sk-abc"}}},
    };
    auto auth_resp = adapter.handle_request(auth_req);
    ASSERT_EQ(auth_resp["status"], "ok");
    const auto sid = auth_resp["session_id"].get<std::string>();
    EXPECT_FALSE(sid.empty());

    // 2. Subsequent call with session_id passes auth.  new_session mints
    //    a fresh opaque session id bound to the same auth method.
    nlohmann::json call = {
        {"method", "new_session"},
        {"session_id", sid},
    };
    auto r = adapter.handle_request(call);
    EXPECT_EQ(r["status"], "ok");
    EXPECT_TRUE(r.contains("session_id"));
    EXPECT_FALSE(r["session_id"].get<std::string>().empty());
    EXPECT_NE(r["session_id"].get<std::string>(), sid);
}

TEST(AcpAuthTest, AuthenticateRpcErrorPropagated) {
    AcpAdapter adapter(empty_env_config());
    nlohmann::json auth_req = {
        {"method", "authenticate"},
        {"method_id", "oauth"},
        {"params", nlohmann::json::object()},
    };
    auto resp = adapter.handle_request(auth_req);
    EXPECT_EQ(resp["status"], "error");
    EXPECT_NE(resp["error"].get<std::string>().find("missing"),
              std::string::npos);
}

TEST(AcpAuthTest, SessionIdsAreUnique) {
    AcpAdapter adapter(empty_env_config());
    nlohmann::json p = {{"api_key", "k"}};
    auto a = adapter.authenticate("api-key", p);
    auto b = adapter.authenticate("api-key", p);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    EXPECT_NE(a.session_id, b.session_id);
}

}  // namespace
}  // namespace hermes::acp
