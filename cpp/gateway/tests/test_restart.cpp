#include <gtest/gtest.h>

#include <hermes/gateway/restart.hpp>

#include <chrono>

namespace hg = hermes::gateway;

class RestartTest : public ::testing::Test {
protected:
    void SetUp() override { hg::clear_restart_request(); }
    void TearDown() override { hg::clear_restart_request(); }
};

TEST_F(RestartTest, RequestRestartSetsFlag) {
    EXPECT_FALSE(hg::restart_requested());
    hg::request_restart();
    EXPECT_TRUE(hg::restart_requested());
}

TEST_F(RestartTest, RestartRequestedReturnsFlag) {
    EXPECT_FALSE(hg::restart_requested());
    hg::request_restart();
    EXPECT_TRUE(hg::restart_requested());
    hg::clear_restart_request();
    EXPECT_FALSE(hg::restart_requested());
}

TEST_F(RestartTest, DrainTimeoutConstantExists) {
    constexpr auto timeout = hg::DRAIN_TIMEOUT;
    EXPECT_EQ(timeout, std::chrono::seconds(30));
    EXPECT_GT(timeout, std::chrono::seconds(0));
}

TEST_F(RestartTest, RepeatedRequestIdempotent) {
    hg::request_restart();
    hg::request_restart();
    hg::request_restart();
    EXPECT_TRUE(hg::restart_requested());
    hg::clear_restart_request();
    EXPECT_FALSE(hg::restart_requested());
}
