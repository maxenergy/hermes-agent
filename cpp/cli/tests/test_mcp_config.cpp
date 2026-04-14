// Tests for hermes::cli::mcp_config.
#include "hermes/cli/mcp_config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hermes::cli::mcp_config;

TEST(MCPConfig, EnvKeyForServer_UpperAndSanitise) {
    EXPECT_EQ(env_key_for_server("ink"), "MCP_INK_API_KEY");
    EXPECT_EQ(env_key_for_server("github-models"),
              "MCP_GITHUB_MODELS_API_KEY");
    EXPECT_EQ(env_key_for_server("my.weird.name"),
              "MCP_MY_WEIRD_NAME_API_KEY");
}

TEST(MCPConfig, InterpolateEnv_ReplacesBracedRefs) {
    ::setenv("MCP_TEST_KEY", "abc123", 1);
    EXPECT_EQ(interpolate_env("Bearer ${MCP_TEST_KEY}"), "Bearer abc123");
    EXPECT_EQ(interpolate_env("no refs"), "no refs");
    // Missing env var becomes empty.
    ::unsetenv("MCP_MISSING_XYZ");
    EXPECT_EQ(interpolate_env("val=${MCP_MISSING_XYZ}"), "val=");
}

TEST(MCPConfig, MaskAuthValue_ShortAndLong) {
    ::setenv("MCP_LONGVAL", "secretvalue12345", 1);
    EXPECT_EQ(mask_auth_value("${MCP_LONGVAL}"), "secr***2345");
    EXPECT_EQ(mask_auth_value("short"), "***");
}

TEST(MCPConfig, ServerConfigRoundTrip_HTTP) {
    nlohmann::json j;
    j["url"] = "https://mcp.example.com/mcp";
    j["headers"] = {{"Authorization", "Bearer ${MCP_FOO_API_KEY}"}};
    j["enabled"] = true;
    j["tools"] = {{"include", nlohmann::json::array({"search", "fetch"})}};
    auto c = ServerConfig::from_json("foo", j);
    EXPECT_TRUE(c.is_http());
    EXPECT_EQ(c.url, "https://mcp.example.com/mcp");
    EXPECT_EQ(c.include.size(), 2u);
    EXPECT_EQ(c.headers.at("Authorization"),
              "Bearer ${MCP_FOO_API_KEY}");
    auto roundtrip = c.to_json();
    EXPECT_EQ(roundtrip["url"], "https://mcp.example.com/mcp");
    EXPECT_EQ(roundtrip["enabled"], true);
}

TEST(MCPConfig, ServerConfigRoundTrip_Stdio) {
    nlohmann::json j;
    j["command"] = "npx";
    j["args"] = nlohmann::json::array({"@modelcontextprotocol/server-github"});
    j["enabled"] = false;
    auto c = ServerConfig::from_json("gh", j);
    EXPECT_TRUE(c.is_stdio());
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.args.size(), 1u);
}

TEST(MCPConfig, RenderRow_FormatsTransport) {
    ServerConfig c;
    c.name = "foo";
    c.url = "https://very.long.example.domain.net/mcp";
    c.include = {"one", "two"};
    c.enabled = true;
    auto row = render_row(c);
    EXPECT_EQ(row.name, "foo");
    EXPECT_LE(row.transport.size(), 28u);
    EXPECT_EQ(row.tools_str, "2 selected");
    EXPECT_EQ(row.status, "enabled");
}

TEST(MCPConfig, RenderRow_StdioShowsCommand) {
    ServerConfig c;
    c.name = "gh";
    c.command = "npx";
    c.args = {"@pkg/server", "--flag"};
    c.enabled = false;
    auto row = render_row(c);
    EXPECT_NE(row.transport.find("npx"), std::string::npos);
    EXPECT_EQ(row.status, "disabled");
    EXPECT_EQ(row.tools_str, "all");
}

TEST(MCPConfig, ProbeServer_StdioCommandNotOnPath) {
    ServerConfig c;
    c.command = "this-binary-does-not-exist-xyz";
    auto r = probe_server(c);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.message.find("not on PATH"), std::string::npos);
}

TEST(MCPConfig, ProbeServer_HTTPUnreachableHost) {
    ServerConfig c;
    c.url = "http://invalid.local.nope.example:59555/";
    auto r = probe_server(c);
    EXPECT_FALSE(r.ok);
}
