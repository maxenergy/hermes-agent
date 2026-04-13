#include "hermes/acp/acp_adapter.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::acp {
namespace {

TEST(AcpAdapterTest, CapabilitiesReturnsJson) {
    AcpConfig config;
    config.listen_address = "127.0.0.1";
    config.listen_port = 9999;

    AcpAdapter adapter(config);
    auto caps = adapter.capabilities();

    EXPECT_EQ(caps["name"], "hermes");
    EXPECT_EQ(caps["protocol"], "acp");
    EXPECT_EQ(caps["listen_port"], 9999);
    EXPECT_TRUE(caps.contains("capabilities"));
    EXPECT_TRUE(caps["capabilities"]["chat"].get<bool>());
    EXPECT_TRUE(caps.contains("auth_methods"));
    EXPECT_TRUE(caps["auth_methods"].is_array());
    EXPECT_GE(caps["auth_methods"].size(), 2u);
}

TEST(AcpAdapterTest, HandleRequestUnauthenticatedIsRejected) {
    // No env token forced -> calls without auth must be rejected.
    AcpConfig cfg;
    cfg.forced_env_token = "";  // explicit empty to ignore real env
    AcpAdapter adapter(cfg);

    nlohmann::json request = {{"method", "some_action"}};
    auto result = adapter.handle_request(request);

    EXPECT_EQ(result["status"], "error");
    EXPECT_EQ(result["error"], "unauthenticated");
}

TEST(AcpAdapterTest, HandleRequestCapabilities) {
    AcpAdapter adapter(AcpConfig{});

    nlohmann::json request = {{"method", "capabilities"}};
    auto result = adapter.handle_request(request);

    EXPECT_EQ(result["name"], "hermes");
    EXPECT_TRUE(result.contains("capabilities"));
}

TEST(AcpAdapterTest, StartStopLifecycle) {
    AcpAdapter adapter(AcpConfig{});

    EXPECT_FALSE(adapter.running());

    adapter.start();
    EXPECT_TRUE(adapter.running());

    adapter.stop();
    EXPECT_FALSE(adapter.running());
}

}  // namespace
}  // namespace hermes::acp
