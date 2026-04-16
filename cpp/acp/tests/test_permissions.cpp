// hermes::acp::PermissionMatrix + ACP-adapter permission RPC tests.
#include "hermes/acp/acp_adapter.hpp"
#include "hermes/acp/permissions.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::acp {
namespace {

AcpConfig empty_env_config() {
    AcpConfig c;
    c.forced_env_token = "";
    return c;
}

// Mint an authenticated session id for RPC tests.
std::string mint_session(AcpAdapter& adapter) {
    nlohmann::json params = {{"api_key", "sk-test-matrix"}};
    auto r = adapter.authenticate("api-key", params);
    EXPECT_TRUE(r.ok) << r.error;
    return r.session_id;
}

// --- PermissionMatrix core ------------------------------------------------

TEST(PermissionMatrixTest, DefaultDecisionsAreConservative) {
    PermissionMatrix m;
    EXPECT_EQ(m.evaluate(PermissionScope::FsRead, nlohmann::json::object()),
              PermissionDecision::Allow);
    EXPECT_EQ(m.evaluate(PermissionScope::MemoryWrite, nlohmann::json::object()),
              PermissionDecision::Allow);
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite, nlohmann::json::object()),
              PermissionDecision::AskUser);
    EXPECT_EQ(m.evaluate(PermissionScope::TerminalExec, nlohmann::json::object()),
              PermissionDecision::AskUser);
    EXPECT_EQ(m.evaluate(PermissionScope::NetFetch, nlohmann::json::object()),
              PermissionDecision::AskUser);
    EXPECT_EQ(m.evaluate(PermissionScope::SkillInvoke, nlohmann::json::object()),
              PermissionDecision::AskUser);
}

TEST(PermissionMatrixTest, SetDefaultOverridesEvaluation) {
    PermissionMatrix m;
    m.set_default(PermissionScope::FsWrite, PermissionDecision::Allow);
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite, nlohmann::json::object()),
              PermissionDecision::Allow);
}

TEST(PermissionMatrixTest, PatternRuleMatchesUrlHost) {
    PermissionMatrix m;
    m.add_rule({PermissionScope::NetFetch, PermissionDecision::Deny,
                "*evil.com/*"});

    nlohmann::json bad = {{"url", "https://evil.com/foo"}};
    EXPECT_EQ(m.evaluate(PermissionScope::NetFetch, bad),
              PermissionDecision::Deny);

    nlohmann::json good = {{"url", "https://good.com"}};
    EXPECT_EQ(m.evaluate(PermissionScope::NetFetch, good),
              PermissionDecision::AskUser);
}

TEST(PermissionMatrixTest, PatternRuleForFilesystemPath) {
    PermissionMatrix m;
    m.add_rule({PermissionScope::FsWrite, PermissionDecision::Allow,
                "/tmp/*"});
    m.add_rule({PermissionScope::FsWrite, PermissionDecision::Deny,
                "/etc/*"});

    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite,
                         nlohmann::json{{"path", "/tmp/scratch.txt"}}),
              PermissionDecision::Allow);
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite,
                         nlohmann::json{{"path", "/etc/passwd"}}),
              PermissionDecision::Deny);
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite,
                         nlohmann::json{{"path", "/home/me/data"}}),
              PermissionDecision::AskUser);
}

TEST(PermissionMatrixTest, EmptyPatternMatchesScopeWide) {
    PermissionMatrix m;
    m.add_rule({PermissionScope::TerminalExec, PermissionDecision::Deny, ""});
    EXPECT_EQ(m.evaluate(PermissionScope::TerminalExec,
                         nlohmann::json{{"command", "ls"}}),
              PermissionDecision::Deny);
    EXPECT_EQ(m.evaluate(PermissionScope::TerminalExec,
                         nlohmann::json::object()),
              PermissionDecision::Deny);
}

TEST(PermissionMatrixTest, FirstMatchingRuleWins) {
    PermissionMatrix m;
    m.add_rule({PermissionScope::NetFetch, PermissionDecision::Allow,
                "*.example.com/*"});
    m.add_rule({PermissionScope::NetFetch, PermissionDecision::Deny,
                "*evil*"});

    EXPECT_EQ(m.evaluate(PermissionScope::NetFetch,
                         nlohmann::json{{"url", "evil.example.com/hack"}}),
              PermissionDecision::Allow);
}

TEST(PermissionMatrixTest, ClearRemovesRulesButKeepsDefaults) {
    PermissionMatrix m;
    m.add_rule({PermissionScope::FsWrite, PermissionDecision::Allow, ""});
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite, nlohmann::json::object()),
              PermissionDecision::Allow);
    m.clear();
    EXPECT_EQ(m.evaluate(PermissionScope::FsWrite, nlohmann::json::object()),
              PermissionDecision::AskUser);
}

// --- JSON round-trip ------------------------------------------------------

TEST(PermissionMatrixJsonTest, RoundTripPreservesState) {
    PermissionMatrix m;
    m.set_default(PermissionScope::FsRead, PermissionDecision::Deny);
    m.add_rule({PermissionScope::NetFetch, PermissionDecision::Deny,
                "evil.com/*"});
    m.add_rule({PermissionScope::FsWrite, PermissionDecision::Allow,
                "/tmp/*"});

    auto payload = m.to_json();
    auto parsed  = PermissionMatrix::from_json(payload);
    auto payload2 = parsed.to_json();

    EXPECT_EQ(payload, payload2);

    EXPECT_EQ(parsed.evaluate(PermissionScope::FsRead, nlohmann::json::object()),
              PermissionDecision::Deny);
    EXPECT_EQ(parsed.evaluate(PermissionScope::NetFetch,
                              nlohmann::json{{"url", "evil.com/x"}}),
              PermissionDecision::Deny);
    EXPECT_EQ(parsed.evaluate(PermissionScope::FsWrite,
                              nlohmann::json{{"path", "/tmp/x"}}),
              PermissionDecision::Allow);
}

TEST(PermissionMatrixJsonTest, StableKeyFormat) {
    PermissionMatrix m;
    auto j = m.to_json();
    ASSERT_TRUE(j.contains("defaults"));
    ASSERT_TRUE(j.contains("rules"));
    EXPECT_EQ(j["defaults"]["fs_read"], "allow");
    EXPECT_EQ(j["defaults"]["fs_write"], "ask_user");
    EXPECT_EQ(j["defaults"]["terminal_exec"], "ask_user");
    EXPECT_EQ(j["defaults"]["net_fetch"], "ask_user");
    EXPECT_EQ(j["defaults"]["memory_write"], "allow");
    EXPECT_EQ(j["defaults"]["skill_invoke"], "ask_user");
    EXPECT_TRUE(j["rules"].is_array());
    EXPECT_TRUE(j["rules"].empty());
}

TEST(PermissionMatrixJsonTest, IgnoresUnknownScopeInInput) {
    nlohmann::json j = {
        {"defaults", {{"fs_read", "deny"}, {"imaginary", "allow"}}},
        {"rules", nlohmann::json::array({
            nlohmann::json{{"scope", "nonsense"}, {"decision", "deny"}},
            nlohmann::json{{"scope", "net_fetch"}, {"decision", "deny"},
                           {"pattern", "x"}},
        })},
    };
    auto m = PermissionMatrix::from_json(j);
    EXPECT_EQ(m.evaluate(PermissionScope::FsRead, nlohmann::json::object()),
              PermissionDecision::Deny);
    EXPECT_EQ(m.evaluate(PermissionScope::NetFetch,
                         nlohmann::json{{"url", "x"}}),
              PermissionDecision::Deny);
}

TEST(PermissionMatrixStringTest, EnumStringHelpersRoundTrip) {
    EXPECT_EQ(parse_scope(to_string(PermissionScope::FsRead)),
              PermissionScope::FsRead);
    EXPECT_EQ(parse_decision(to_string(PermissionDecision::AskUser)),
              PermissionDecision::AskUser);
    EXPECT_THROW(parse_scope("bogus"), std::invalid_argument);
    EXPECT_THROW(parse_decision("maybe"), std::invalid_argument);
}

// --- AcpAdapter RPC integration -------------------------------------------

TEST(AcpPermissionRpcTest, SetAndGetPermissions) {
    AcpAdapter adapter(empty_env_config());
    std::string sid = mint_session(adapter);

    // Default matrix read — session exists, no matrix installed yet.
    nlohmann::json get_req = {
        {"method", "session/get_permissions"},
        {"session_id", sid},
    };
    auto get_resp = adapter.handle_request(get_req);
    ASSERT_EQ(get_resp["status"], "ok") << get_resp.dump();
    ASSERT_TRUE(get_resp["matrix"].is_object());
    EXPECT_EQ(get_resp["matrix"]["defaults"]["terminal_exec"], "ask_user");

    // Install a custom matrix that denies fetches to evil.com.
    nlohmann::json matrix = {
        {"defaults", {{"fs_read", "allow"},
                      {"fs_write", "ask_user"},
                      {"terminal_exec", "deny"},
                      {"net_fetch", "ask_user"},
                      {"memory_write", "allow"},
                      {"skill_invoke", "ask_user"}}},
        {"rules", nlohmann::json::array({
            nlohmann::json{{"scope", "net_fetch"},
                           {"decision", "deny"},
                           {"pattern", "evil.com/*"}},
        })},
    };

    nlohmann::json set_req = {
        {"method", "session/set_permissions"},
        {"session_id", sid},
        {"params", {{"matrix", matrix}}},
    };
    auto set_resp = adapter.handle_request(set_req);
    ASSERT_EQ(set_resp["status"], "ok") << set_resp.dump();

    // Verify the stored matrix via the adapter helper.
    auto decision =
        adapter.check_permission(sid, PermissionScope::NetFetch,
                                 nlohmann::json{{"url", "evil.com/bad"}});
    EXPECT_EQ(decision, PermissionDecision::Deny);

    auto decision2 =
        adapter.check_permission(sid, PermissionScope::TerminalExec,
                                 nlohmann::json{{"command", "ls"}});
    EXPECT_EQ(decision2, PermissionDecision::Deny);

    // get_permissions round-trip reflects the install.
    auto get_resp2 = adapter.handle_request(get_req);
    ASSERT_EQ(get_resp2["status"], "ok");
    EXPECT_EQ(get_resp2["matrix"]["defaults"]["terminal_exec"], "deny");
    ASSERT_TRUE(get_resp2["matrix"]["rules"].is_array());
    ASSERT_EQ(get_resp2["matrix"]["rules"].size(), 1u);
    EXPECT_EQ(get_resp2["matrix"]["rules"][0]["pattern"], "evil.com/*");
}

TEST(AcpPermissionRpcTest, UnknownSessionReturnsInvalidParams) {
    AcpAdapter adapter(empty_env_config());
    std::string sid = mint_session(adapter);  // valid outer session

    // Reference a different session_id inside params.
    nlohmann::json req = {
        {"method", "session/get_permissions"},
        {"session_id", sid},
        {"params", {{"session_id", "does-not-exist"}}},
    };
    auto resp = adapter.handle_request(req);
    EXPECT_EQ(resp["status"], "error");
    EXPECT_EQ(resp["error"], "invalid_params");
    EXPECT_EQ(resp["code"], -32602);
}

TEST(AcpPermissionRpcTest, SetPermissionsMissingMatrixRejected) {
    AcpAdapter adapter(empty_env_config());
    std::string sid = mint_session(adapter);

    nlohmann::json req = {
        {"method", "session/set_permissions"},
        {"session_id", sid},
        {"params", nlohmann::json::object()},
    };
    auto resp = adapter.handle_request(req);
    EXPECT_EQ(resp["status"], "error");
    EXPECT_EQ(resp["error"], "invalid_params");
    EXPECT_EQ(resp["code"], -32602);
}

TEST(AcpPermissionRpcTest, CheckPermissionFallsBackToDefaultMatrix) {
    AcpAdapter adapter(empty_env_config());
    std::string sid = mint_session(adapter);

    // No matrix installed for this session — should use conservative
    // defaults.
    EXPECT_FALSE(adapter.has_permission_matrix(sid));
    EXPECT_EQ(
        adapter.check_permission(sid, PermissionScope::FsRead,
                                 nlohmann::json::object()),
        PermissionDecision::Allow);
    EXPECT_EQ(
        adapter.check_permission(sid, PermissionScope::TerminalExec,
                                 nlohmann::json::object()),
        PermissionDecision::AskUser);
}

TEST(AcpPermissionRpcTest, SessionCloseClearsMatrix) {
    AcpAdapter adapter(empty_env_config());
    std::string sid = mint_session(adapter);

    PermissionMatrix m;
    m.add_rule({PermissionScope::FsWrite, PermissionDecision::Deny, ""});
    adapter.set_permissions(sid, std::move(m));
    EXPECT_TRUE(adapter.has_permission_matrix(sid));

    nlohmann::json close_req = {{"method", "session/close"},
                                {"session_id", sid}};
    auto close_resp = adapter.handle_request(close_req);
    ASSERT_EQ(close_resp["status"], "ok");
    EXPECT_FALSE(adapter.has_permission_matrix(sid));
}

}  // namespace
}  // namespace hermes::acp
