#include "hermes/core/async_bridge.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using hermes::core::join_all;
using hermes::core::join_all_settled;
using hermes::core::run_async;
using hermes::core::wait_for_all;

TEST(AsyncBridge, RunAsyncReturnsValue) {
    auto f = run_async([] { return 42; });
    EXPECT_EQ(f.get(), 42);
}

TEST(AsyncBridge, RunAsyncForwardsArgs) {
    auto f = run_async([](int a, int b) { return a + b; }, 3, 4);
    EXPECT_EQ(f.get(), 7);
}

TEST(AsyncBridge, JoinAllPreservesOrder) {
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 8; ++i) {
        futs.emplace_back(run_async([i] {
            // Stagger completion order so the test would fail if
            // join_all re-ordered results.
            std::this_thread::sleep_for(std::chrono::milliseconds((8 - i) * 2));
            return i * i;
        }));
    }
    auto result = join_all(std::move(futs));
    ASSERT_EQ(result.size(), 8u);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(result[i], i * i);
}

TEST(AsyncBridge, JoinAllVoid) {
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 5; ++i) {
        futs.emplace_back(run_async([&counter] { ++counter; }));
    }
    join_all(std::move(futs));
    EXPECT_EQ(counter.load(), 5);
}

TEST(AsyncBridge, JoinAllRethrows) {
    std::vector<std::future<int>> futs;
    futs.emplace_back(run_async([] { return 1; }));
    futs.emplace_back(run_async([]() -> int { throw std::runtime_error("boom"); }));
    EXPECT_THROW(join_all(std::move(futs)), std::runtime_error);
}

TEST(AsyncBridge, JoinAllSettledCapturesErrors) {
    std::vector<std::future<int>> futs;
    futs.emplace_back(run_async([] { return 10; }));
    futs.emplace_back(run_async([]() -> int { throw std::runtime_error("nope"); }));
    futs.emplace_back(run_async([] { return 20; }));
    auto outs = join_all_settled(std::move(futs));
    ASSERT_EQ(outs.size(), 3u);
    EXPECT_TRUE(outs[0].ok);
    EXPECT_EQ(outs[0].value, 10);
    EXPECT_FALSE(outs[1].ok);
    EXPECT_NE(outs[1].error.find("nope"), std::string::npos);
    EXPECT_TRUE(outs[2].ok);
    EXPECT_EQ(outs[2].value, 20);
}

TEST(AsyncBridge, WaitForAllRespectsDeadline) {
    std::vector<std::future<int>> futs;
    futs.emplace_back(run_async([] { return 1; }));
    futs.emplace_back(run_async([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 2;
    }));
    auto done = wait_for_all(futs, std::chrono::milliseconds(100));
    EXPECT_LE(done, 1u);  // the long sleeper should not finish in 100ms
    // Drain to avoid blocking the destructor for too long in CI — let
    // the long future detach via std::async's shared state.
}
