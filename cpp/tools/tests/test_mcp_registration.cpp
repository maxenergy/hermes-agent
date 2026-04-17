// Tests for the MCP registration surface (tools/mcp_registration.cpp).
//
// These exercise the pure helpers used by the registration half of the
// MCP client — schema translation, allow/deny filtering, utility
// selection, ledger bookkeeping, env sanitization, and credential
// redaction.
#include "hermes/tools/mcp_registration.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>
#include <unordered_set>

using namespace hermes::tools;
using namespace hermes::tools::mcp;
using json = nlohmann::json;

// -------------------------------------------------------------------------
// Name sanitization + prefixing
// -------------------------------------------------------------------------

TEST(McpRegistrationName, Sanitize) {
    EXPECT_EQ(sanitize_name_component("foo-bar"), "foo_bar");
    EXPECT_EQ(sanitize_name_component("ok_123"), "ok_123");
    EXPECT_EQ(sanitize_name_component("a.b/c d"), "a_b_c_d");
    EXPECT_EQ(sanitize_name_component(""), "");
}

TEST(McpRegistrationName, Prefix) {
    EXPECT_EQ(make_prefixed_name("github", "list-files"),
              "mcp_github_list_files");
    EXPECT_EQ(make_prefixed_name("my.server", "tool-1"),
              "mcp_my_server_tool_1");
}

// -------------------------------------------------------------------------
// Schema normalization + translation
// -------------------------------------------------------------------------

TEST(McpRegistrationSchema, NormalizeNull) {
    auto j = normalize_input_schema(json());
    EXPECT_EQ(j["type"], "object");
    EXPECT_TRUE(j.contains("properties"));
    EXPECT_TRUE(j["properties"].is_object());
}

TEST(McpRegistrationSchema, NormalizeObjectWithoutProps) {
    json in = {{"type", "object"}};
    auto j = normalize_input_schema(in);
    EXPECT_TRUE(j.contains("properties"));
}

TEST(McpRegistrationSchema, NormalizePassesThrough) {
    json in = {{"type", "object"},
               {"properties", {{"x", {{"type", "string"}}}}}};
    auto j = normalize_input_schema(in);
    EXPECT_EQ(j, in);
}

TEST(McpRegistrationSchema, ConvertToolSchema) {
    json mcp_tool = {
        {"name", "list-files"},
        {"description", "List files"},
        {"inputSchema", {{"type", "object"},
                         {"properties", {{"path", {{"type", "string"}}}}}}},
    };
    auto schema = convert_tool_schema("github", mcp_tool);
    EXPECT_EQ(schema["name"], "mcp_github_list_files");
    EXPECT_EQ(schema["description"], "List files");
    EXPECT_EQ(schema["parameters"]["type"], "object");
}

TEST(McpRegistrationSchema, ConvertToolSchemaEmptyDescription) {
    json mcp_tool = {{"name", "t"}};
    auto schema = convert_tool_schema("s", mcp_tool);
    EXPECT_NE(schema["description"].get<std::string>().find("MCP tool t"),
              std::string::npos);
}

// -------------------------------------------------------------------------
// Filter
// -------------------------------------------------------------------------

TEST(McpRegistrationFilter, EmptyAcceptsAll) {
    ToolFilter f;
    EXPECT_TRUE(f.accepts("anything"));
    EXPECT_TRUE(f.accepts("else"));
}

TEST(McpRegistrationFilter, IncludeWhitelist) {
    ToolFilter f;
    f.include = {"a", "b"};
    EXPECT_TRUE(f.accepts("a"));
    EXPECT_FALSE(f.accepts("c"));
}

TEST(McpRegistrationFilter, ExcludeBlacklist) {
    ToolFilter f;
    f.exclude = {"bad"};
    EXPECT_TRUE(f.accepts("good"));
    EXPECT_FALSE(f.accepts("bad"));
}

TEST(McpRegistrationFilter, IncludeWinsOverExclude) {
    ToolFilter f;
    f.include = {"a"};
    f.exclude = {"a"};
    EXPECT_TRUE(f.accepts("a"));
    EXPECT_FALSE(f.accepts("b"));
}

TEST(McpRegistrationFilter, FromJson) {
    json block = {
        {"include", json::array({"x", "y"})},
        {"exclude", "z"},
        {"resources", false},
        {"prompts", "on"},
    };
    auto f = ToolFilter::from_json(block);
    EXPECT_EQ(f.include.size(), 2u);
    EXPECT_EQ(f.exclude.size(), 1u);
    EXPECT_FALSE(f.resources_enabled);
    EXPECT_TRUE(f.prompts_enabled);
}

TEST(McpRegistrationFilter, ParseBoolish) {
    EXPECT_TRUE(parse_boolish(json(true)));
    EXPECT_TRUE(parse_boolish(json("yes")));
    EXPECT_FALSE(parse_boolish(json("off")));
    EXPECT_TRUE(parse_boolish(json(1)));
    EXPECT_FALSE(parse_boolish(json(0)));
    EXPECT_TRUE(parse_boolish(json(), /*default_value=*/true));
}

TEST(McpRegistrationFilter, NormalizeNameFilterString) {
    auto s = normalize_name_filter(json("one"));
    EXPECT_EQ(s.size(), 1u);
    EXPECT_TRUE(s.count("one"));
}

TEST(McpRegistrationFilter, NormalizeNameFilterArray) {
    auto s = normalize_name_filter(json::array({"a", "b", "a"}));
    EXPECT_EQ(s.size(), 2u);
}

// -------------------------------------------------------------------------
// Utility schemas
// -------------------------------------------------------------------------

TEST(McpRegistrationUtility, BuildAll) {
    auto us = build_utility_schemas("srv");
    ASSERT_EQ(us.size(), 4u);
    EXPECT_EQ(us[0].schema["name"], "mcp_srv_list_resources");
    EXPECT_EQ(us[1].schema["name"], "mcp_srv_read_resource");
    EXPECT_EQ(us[2].schema["name"], "mcp_srv_list_prompts");
    EXPECT_EQ(us[3].schema["name"], "mcp_srv_get_prompt");
}

TEST(McpRegistrationUtility, SelectFilteredByFlags) {
    ToolFilter f;
    f.resources_enabled = false;
    std::unordered_set<std::string> caps = {"list_resources", "list_prompts",
                                           "read_resource", "get_prompt"};
    auto us = select_utility_schemas("s", f, caps);
    for (const auto& u : us) {
        EXPECT_NE(u.handler_key, "list_resources");
        EXPECT_NE(u.handler_key, "read_resource");
    }
}

TEST(McpRegistrationUtility, SelectFilteredByCapabilities) {
    ToolFilter f;
    std::unordered_set<std::string> caps = {"list_prompts"};
    auto us = select_utility_schemas("s", f, caps);
    ASSERT_EQ(us.size(), 1u);
    EXPECT_EQ(us[0].handler_key, "list_prompts");
}

// -------------------------------------------------------------------------
// Ledger
// -------------------------------------------------------------------------

TEST(McpRegistrationLedger, AddAndQuery) {
    ServerToolLedger l;
    l.add("a", "mcp_a_t1");
    l.add("a", "mcp_a_t2");
    l.add("b", "mcp_b_t1");
    auto a_tools = l.tools_for("a");
    ASSERT_EQ(a_tools.size(), 2u);
    EXPECT_EQ(l.all_tools().size(), 3u);
    EXPECT_TRUE(l.has("a"));
    EXPECT_FALSE(l.has("c"));
    EXPECT_EQ(l.servers().size(), 2u);
}

TEST(McpRegistrationLedger, ClearServer) {
    ServerToolLedger l;
    l.add("x", "t1");
    l.clear_server("x");
    EXPECT_FALSE(l.has("x"));
    EXPECT_TRUE(l.tools_for("x").empty());
}

// -------------------------------------------------------------------------
// Plan + Apply
// -------------------------------------------------------------------------

TEST(McpRegistrationPlan, BasicPlan) {
    std::vector<json> tools = {
        {{"name", "t1"}, {"description", "d1"}},
        {{"name", "t2"}, {"description", "d2"}},
        {{"name", ""}},  // skipped (empty name)
    };
    ToolFilter f;
    f.exclude = {"t2"};
    std::unordered_set<std::string> caps;  // no utilities
    auto plan = plan_registration("srv", tools, f, caps);
    ASSERT_EQ(plan.tool_schemas.size(), 1u);
    EXPECT_EQ(plan.tool_schemas[0]["name"], "mcp_srv_t1");
    EXPECT_EQ(plan.skipped.size(), 1u);
    EXPECT_EQ(plan.skipped[0], "t2");
}

TEST(McpRegistrationPlan, ApplyPopulatesRegistry) {
    auto& reg = ToolRegistry::instance();
    // Clean any prior state that might collide with our test name.
    reg.deregister("mcp_srv_echo");
    ServerToolLedger ledger;
    std::vector<json> tools = {
        {{"name", "echo"}, {"description", "echo back"}},
    };
    auto plan = plan_registration("srv", tools, ToolFilter{}, {});
    McpHandlerFactory factory = [](const std::string&, const std::string&,
                                    const std::string&) -> ToolHandler {
        return [](const json&, const ToolContext&) {
            return std::string("\"ok\"");
        };
    };
    auto n = apply_registration(plan, reg, ledger, factory);
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(reg.has_tool("mcp_srv_echo"));
    EXPECT_TRUE(ledger.has("srv"));
    EXPECT_EQ(ledger.tools_for("srv").size(), 1u);
    // Dispatch and verify handler was bound.
    auto res = reg.dispatch("mcp_srv_echo", json::object(), ToolContext{});
    EXPECT_NE(res.find("ok"), std::string::npos);
}

TEST(McpRegistrationPlan, NullFactoryFallbackIsInformative) {
    // Covers the defensive branch in apply_registration() where a caller
    // passes a null McpHandlerFactory.  The handler must not return the
    // opaque legacy "no handler bound" message — it should report the
    // actual root cause (client not connected) along with the server /
    // tool identity so logs and the model both see a useful error.
    auto& reg = ToolRegistry::instance();
    reg.deregister("mcp_proxy_ping");
    reg.deregister("mcp_proxy_list_resources");
    ServerToolLedger ledger;
    std::vector<json> tools = {
        {{"name", "ping"}, {"description", "ping"}},
    };
    std::unordered_set<std::string> caps = {"list_resources"};
    auto plan = plan_registration("proxy", tools, ToolFilter{}, caps);
    auto n = apply_registration(plan, reg, ledger, /*make_handler=*/{});
    EXPECT_EQ(n, 2u);  // 1 tool + 1 utility (list_resources)

    auto tool_res = reg.dispatch("mcp_proxy_ping", json::object(),
                                 ToolContext{});
    // Must NOT contain the legacy opaque string.
    EXPECT_EQ(tool_res.find("no handler bound"), std::string::npos);
    EXPECT_NE(tool_res.find("not connected"), std::string::npos);
    EXPECT_NE(tool_res.find("proxy"), std::string::npos);
    EXPECT_NE(tool_res.find("ping"), std::string::npos);

    auto util_res = reg.dispatch("mcp_proxy_list_resources",
                                 json::object(), ToolContext{});
    EXPECT_EQ(util_res.find("no handler bound"), std::string::npos);
    EXPECT_NE(util_res.find("not connected"), std::string::npos);
    EXPECT_NE(util_res.find("list_resources"), std::string::npos);
}

// -------------------------------------------------------------------------
// Diff
// -------------------------------------------------------------------------

TEST(McpRegistrationDiff, AddedAndRemoved) {
    auto d = diff_tool_lists({"a", "b", "c"}, {"b", "c", "d"});
    ASSERT_EQ(d.added.size(), 1u);
    EXPECT_EQ(d.added[0], "d");
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0], "a");
}

TEST(McpRegistrationDiff, Identical) {
    auto d = diff_tool_lists({"a", "b"}, {"a", "b"});
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
}

// -------------------------------------------------------------------------
// Env sanitization / error redaction / interpolation
// -------------------------------------------------------------------------

TEST(McpRegistrationEnv, BuildSafeEnvFilters) {
    std::unordered_map<std::string, std::string> env = {
        {"PATH", "/usr/bin"},
        {"SECRET", "no"},
        {"MCP_FOO", "yes"},
        {"HOME", "/home/u"},
    };
    auto out = build_safe_env(env);
    EXPECT_TRUE(out.count("PATH"));
    EXPECT_TRUE(out.count("HOME"));
    EXPECT_TRUE(out.count("MCP_FOO"));
    EXPECT_FALSE(out.count("SECRET"));
}

TEST(McpRegistrationEnv, BuildSafeEnvExtraOverrides) {
    std::unordered_map<std::string, std::string> env = {{"PATH", "/a"}};
    std::unordered_map<std::string, std::string> extra = {
        {"PATH", "/b"}, {"CUSTOM", "x"}};
    auto out = build_safe_env(env, extra);
    EXPECT_EQ(out["PATH"], "/b");
    EXPECT_EQ(out["CUSTOM"], "x");
}

TEST(McpRegistrationErr, RedactsBearer) {
    std::string s = "Authorization: Bearer abc.def.ghi";
    auto r = sanitize_error(s);
    EXPECT_NE(r.find("Bearer [redacted]"), std::string::npos);
    EXPECT_EQ(r.find("abc.def.ghi"), std::string::npos);
}

TEST(McpRegistrationErr, RedactsGithubPat) {
    auto r = sanitize_error("token ghp_abc123DEF456GHI789jklMNO012pqrSTU345vwx");
    EXPECT_NE(r.find("[redacted-github-token]"), std::string::npos);
}

TEST(McpRegistrationEnv, Interpolate) {
    setenv("HERMES_TEST_VAR", "hello", 1);
    auto r = interpolate_env_vars("prefix ${HERMES_TEST_VAR} suffix");
    EXPECT_EQ(r, "prefix hello suffix");
    unsetenv("HERMES_TEST_VAR");
}

TEST(McpRegistrationEnv, InterpolateMissingLeavesLiteral) {
    unsetenv("HERMES_DEFINITELY_UNSET_XYZ");
    auto r = interpolate_env_vars("a ${HERMES_DEFINITELY_UNSET_XYZ} b");
    EXPECT_NE(r.find("${HERMES_DEFINITELY_UNSET_XYZ}"), std::string::npos);
}

TEST(McpRegistrationEnv, InterpolateDeepObject) {
    setenv("HERMES_DEEP", "VAL", 1);
    json in = {
        {"cmd", "${HERMES_DEEP}"},
        {"args", json::array({"literal", "${HERMES_DEEP}"})},
        {"nested", {{"k", "${HERMES_DEEP}"}}},
        {"num", 42},
    };
    auto out = interpolate_env_vars_deep(in);
    EXPECT_EQ(out["cmd"], "VAL");
    EXPECT_EQ(out["args"][1], "VAL");
    EXPECT_EQ(out["nested"]["k"], "VAL");
    EXPECT_EQ(out["num"], 42);
    unsetenv("HERMES_DEEP");
}
