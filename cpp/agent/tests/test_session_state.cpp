#include "hermes/agent/session_state.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <unordered_set>

using namespace hermes::agent::session_state;

TEST(TurnCounters, RecordsToolCalls) {
    TurnCounters c;
    c.record_tool_call("read_file");
    c.record_tool_call("read_file");
    c.record_tool_call("terminal");
    EXPECT_EQ(c.tool_call_count(), 3);
    auto top = c.top_tools();
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].name, "read_file");
    EXPECT_EQ(top[0].count, 2);
    EXPECT_EQ(top[1].name, "terminal");
    EXPECT_EQ(top[1].count, 1);
}

TEST(TurnCounters, ModelAndErrorCounts) {
    TurnCounters c;
    c.record_model_turn();
    c.record_error();
    c.record_error();
    EXPECT_EQ(c.model_turn_count(), 1);
    EXPECT_EQ(c.error_count(), 2);
}

TEST(TurnCounters, ResetClearsEverything) {
    TurnCounters c;
    c.record_tool_call("x");
    c.record_model_turn();
    c.record_error();
    c.reset();
    EXPECT_EQ(c.tool_call_count(), 0);
    EXPECT_EQ(c.model_turn_count(), 0);
    EXPECT_EQ(c.error_count(), 0);
    EXPECT_TRUE(c.top_tools().empty());
}

TEST(TurnCounters, TopLimitHonoured) {
    TurnCounters c;
    for (int i = 0; i < 15; ++i) {
        c.record_tool_call("t" + std::to_string(i));
    }
    EXPECT_EQ(c.top_tools(5).size(), 5u);
    EXPECT_EQ(c.top_tools(20).size(), 15u);
}

TEST(TurnCounters, ConcurrentIncrements) {
    TurnCounters c;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&c]() {
            for (int i = 0; i < 100; ++i) c.record_tool_call("read_file");
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(c.tool_call_count(), 800);
}

TEST(SessionId, Produces32HexChars) {
    auto id = make_session_id(42);
    EXPECT_EQ(id.size(), 32u);
    for (char c : id) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST(SessionId, DifferentCallsDifferentIds) {
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 100; ++i) seen.insert(make_session_id(42));
    EXPECT_EQ(seen.size(), 100u);
}

TEST(PlatformNormalise, LowersAndStripsSuffix) {
    EXPECT_EQ(normalise_platform_name("Telegram"), "telegram");
    EXPECT_EQ(normalise_platform_name("WEB:room=1"), "web");
    EXPECT_EQ(normalise_platform_name("  cli  "), "cli");
    EXPECT_EQ(normalise_platform_name("SMS,foo"), "sms");
}
