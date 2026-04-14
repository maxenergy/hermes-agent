// Tests for hermes::agent::jittered_backoff.
#include "hermes/agent/retry_utils.hpp"

#include <gtest/gtest.h>

using hermes::agent::jittered_backoff;

TEST(JitteredBackoff, Attempt1UsesBaseDelay) {
    const double d = jittered_backoff(1, 5.0, 120.0, 0.5);
    // min(5, 120) + uniform(0, 2.5) -> [5.0, 7.5]
    EXPECT_GE(d, 5.0);
    EXPECT_LE(d, 7.5 + 1e-9);
}

TEST(JitteredBackoff, Attempt2Doubles) {
    const double d = jittered_backoff(2, 5.0, 120.0, 0.5);
    // 10 + uniform(0,5) -> [10, 15]
    EXPECT_GE(d, 10.0);
    EXPECT_LE(d, 15.0 + 1e-9);
}

TEST(JitteredBackoff, CapRespected) {
    const double d = jittered_backoff(20, 5.0, 120.0, 0.5);
    // delay capped at 120, jitter up to 60 -> [120, 180]
    EXPECT_GE(d, 120.0);
    EXPECT_LE(d, 180.0 + 1e-9);
}

TEST(JitteredBackoff, JitterRatioZeroMeansDeterministic) {
    const double d = jittered_backoff(3, 4.0, 100.0, 0.0);
    // 4 * 2^2 = 16, no jitter
    EXPECT_DOUBLE_EQ(d, 16.0);
}

TEST(JitteredBackoff, HugeExponentReturnsMaxDelay) {
    const double d = jittered_backoff(100, 5.0, 77.0, 0.0);
    EXPECT_DOUBLE_EQ(d, 77.0);
}

TEST(JitteredBackoff, ZeroBaseReturnsMaxDelay) {
    const double d = jittered_backoff(3, 0.0, 50.0, 0.0);
    EXPECT_DOUBLE_EQ(d, 50.0);
}

TEST(JitteredBackoff, TickIncrementsMonotonically) {
    const std::size_t before = hermes::agent::jitter_tick_count_for_testing();
    jittered_backoff(1);
    jittered_backoff(1);
    EXPECT_GE(hermes::agent::jitter_tick_count_for_testing(), before + 2);
}
