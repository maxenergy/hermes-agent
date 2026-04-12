#include "hermes/tools/homeassistant_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

using namespace hermes::tools;

namespace {

class HomeAssistantToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        // Clear env vars first
        unsetenv("HA_URL");
        unsetenv("HA_TOKEN");
        register_homeassistant_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("HA_URL");
        unsetenv("HA_TOKEN");
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(HomeAssistantToolTest, MissingEnvCheckFnFalse) {
    // With no HA_URL/HA_TOKEN, the toolset should be unavailable
    EXPECT_FALSE(ToolRegistry::instance().is_toolset_available("homeassistant"));
}

TEST_F(HomeAssistantToolTest, WithEnvConstructsUrlAndReturnsTransportError) {
    setenv("HA_URL", "http://ha.local:8123", 1);
    setenv("HA_TOKEN", "test_token_abc", 1);

    // Re-register so check_fn picks up env
    ToolRegistry::instance().clear();
    register_homeassistant_tools(ToolRegistry::instance());

    EXPECT_TRUE(ToolRegistry::instance().is_toolset_available("homeassistant"));

    // Dispatch should return the transport-not-available error
    auto r = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "ha_list_entities", nlohmann::json::object(), ctx_));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("HTTP transport"),
              std::string::npos);
}

}  // namespace
