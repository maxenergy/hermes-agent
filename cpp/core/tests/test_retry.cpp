#include "hermes/core/retry.hpp"

#include <chrono>
#include <gtest/gtest.h>

namespace hr = hermes::core::retry;
using namespace std::chrono_literals;

TEST(Retry, GrowsWithAttempt) {
    // First attempt should be at least `base` (jitter is additive).
    const auto d1 = hr::jittered_backoff(1, 100ms, 60s, 0.0);  // no jitter
    EXPECT_GE(d1.count(), 100);
    const auto d3 = hr::jittered_backoff(3, 100ms, 60s, 0.0);
    EXPECT_GE(d3.count(), 400);  // 100 * 2^2 = 400
}

TEST(Retry, CapsAtMaxDelay) {
    const auto d = hr::jittered_backoff(20, 100ms, 5s, 0.0);
    EXPECT_LE(d.count(), 5000);
}

TEST(Retry, AttemptBelowOneClamps) {
    const auto d = hr::jittered_backoff(0, 100ms, 60s, 0.0);
    // attempt<=0 should be treated as attempt=1 (100ms baseline).
    EXPECT_GE(d.count(), 100);
    EXPECT_LE(d.count(), 100 + 200);  // base + jitter headroom
}

TEST(Retry, JitterStaysInRange) {
    // With a ratio of 0.25 and attempt 2, max jitter is base * 0.25 * 2 = 100ms.
    for (int i = 0; i < 20; ++i) {
        const auto d = hr::jittered_backoff(2, 200ms, 60s, 0.25);
        EXPECT_GE(d.count(), 400);
        EXPECT_LE(d.count(), 400 + 100 + 5);  // +5ms slack for floor rounding
    }
}
