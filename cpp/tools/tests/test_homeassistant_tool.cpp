#include "hermes/tools/homeassistant_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

using namespace hermes::tools;
using hermes::llm::FakeHttpTransport;

namespace {

// We need a way to inject FakeHttpTransport into get_default_transport().
// Since HA tools use get_default_transport() internally, we rely on the
// global transport being set up.  For testing purposes, we verify the
// registration and environment checks; HTTP dispatch is tested via the
// pattern used in web_tools/vision_tool tests.

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

TEST_F(HomeAssistantToolTest, AllFourToolsRegistered) {
    auto tools = ToolRegistry::instance().list_tools();
    std::vector<std::string> ha_tools;
    for (const auto& t : tools) {
        if (t.find("ha_") == 0) ha_tools.push_back(t);
    }
    EXPECT_EQ(ha_tools.size(), 4u);
}

TEST_F(HomeAssistantToolTest, WithEnvToolsetAvailable) {
    setenv("HA_URL", "http://ha.local:8123", 1);
    setenv("HA_TOKEN", "test_token_abc", 1);

    // Re-register so check_fn picks up env
    ToolRegistry::instance().clear();
    register_homeassistant_tools(ToolRegistry::instance());

    EXPECT_TRUE(ToolRegistry::instance().is_toolset_available("homeassistant"));
}

TEST_F(HomeAssistantToolTest, GetStateMissingEntityIdReturnsError) {
    setenv("HA_URL", "http://ha.local:8123", 1);
    setenv("HA_TOKEN", "test_token_abc", 1);

    ToolRegistry::instance().clear();
    register_homeassistant_tools(ToolRegistry::instance());

    auto r = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "ha_get_state", nlohmann::json::object(), ctx_));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("entity_id"),
              std::string::npos);
}

TEST_F(HomeAssistantToolTest, CallServiceMissingRequiredParamsReturnsError) {
    setenv("HA_URL", "http://ha.local:8123", 1);
    setenv("HA_TOKEN", "test_token_abc", 1);

    ToolRegistry::instance().clear();
    register_homeassistant_tools(ToolRegistry::instance());

    auto r = nlohmann::json::parse(
        ToolRegistry::instance().dispatch(
            "ha_call_service", nlohmann::json::object(), ctx_));
    EXPECT_TRUE(r.contains("error"));
    EXPECT_NE(r["error"].get<std::string>().find("domain"),
              std::string::npos);
}

}  // namespace
