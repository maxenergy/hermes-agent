#include "hermes/tools/managed_tool_gateway.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using hermes::tools::is_managed_nous_tools_enabled;
using hermes::tools::resolve_vendor_gateway;
using json = nlohmann::json;

TEST(ManagedToolGateway, DisabledByDefault) {
    EXPECT_FALSE(is_managed_nous_tools_enabled(json::object()));
}

TEST(ManagedToolGateway, EmptyConfigReturnsNullopt) {
    auto result = resolve_vendor_gateway("firecrawl", json::object());
    EXPECT_FALSE(result.has_value());
}

TEST(ManagedToolGateway, DisabledExplicitly) {
    json config = {{"managed_tool_gateway", {{"enabled", false}}}};
    EXPECT_FALSE(is_managed_nous_tools_enabled(config));
    auto result = resolve_vendor_gateway("firecrawl", config);
    EXPECT_FALSE(result.has_value());
}
