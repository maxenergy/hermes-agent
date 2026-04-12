#include "hermes/tools/mcp_client_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

using namespace hermes::tools;
using json = nlohmann::json;

namespace {

class McpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(McpClientTest, LoadConfigParsesJson) {
    json config = {
        {"my-server", {
            {"command", "npx"},
            {"args", json::array({"-y", "@mcp/server"})},
            {"timeout", 60},
            {"env", {{"API_KEY", "secret"}}},
            {"sampling", {{"enabled", true}, {"model", "claude-3"}}}
        }},
        {"http-server", {
            {"url", "https://example.com/mcp"},
            {"headers", {{"Authorization", "Bearer tok"}}}
        }}
    };

    McpClientManager mgr;
    mgr.load_config(config);

    auto cfg = mgr.get_config("my-server");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->command, "npx");
    EXPECT_EQ(cfg->args.size(), 2u);
    EXPECT_EQ(cfg->args[0], "-y");
    EXPECT_EQ(cfg->timeout, 60);
    EXPECT_EQ(cfg->env.at("API_KEY"), "secret");
    EXPECT_TRUE(cfg->sampling.enabled);
    EXPECT_EQ(cfg->sampling.model, "claude-3");

    auto cfg2 = mgr.get_config("http-server");
    ASSERT_TRUE(cfg2.has_value());
    EXPECT_EQ(cfg2->url, "https://example.com/mcp");
    EXPECT_EQ(cfg2->headers.at("Authorization"), "Bearer tok");
}

TEST_F(McpClientTest, ServerNamesLists) {
    json config = {
        {"alpha", {{"command", "a"}}},
        {"beta", {{"command", "b"}}},
        {"gamma", {{"url", "http://c"}}}
    };

    McpClientManager mgr;
    mgr.load_config(config);

    auto names = mgr.server_names();
    EXPECT_EQ(names.size(), 3u);

    // std::map keeps sorted order.
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "beta");
    EXPECT_EQ(names[2], "gamma");
}

TEST_F(McpClientTest, RegisterServerToolsCreatesStub) {
    json config = {
        {"test-server", {{"command", "test-cmd"}}}
    };

    McpClientManager mgr;
    mgr.load_config(config);
    mgr.register_server_tools("test-server", ToolRegistry::instance());

    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_test-server"));

    // Dispatching should return an error about not connected.
    auto result = json::parse(ToolRegistry::instance().dispatch(
        "mcp_test-server",
        {{"tool", "some_tool"}, {"arguments", json::object()}},
        ctx_));
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("not connected"),
              std::string::npos);
}

TEST_F(McpClientTest, GetConfigNonexistent) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    EXPECT_FALSE(mgr.get_config("nope").has_value());
    EXPECT_TRUE(mgr.server_names().empty());
}

}  // namespace
