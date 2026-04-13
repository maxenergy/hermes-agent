#include "hermes/agent/insights.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using hermes::agent::InsightEvent;
using hermes::agent::InsightsRecorder;
using hermes::agent::InsightSummary;

namespace {

std::filesystem::path tmp_insights_path() {
    auto dir = std::filesystem::temp_directory_path() /
               ("hermes_insights_" +
                std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir / "insights.jsonl";
}

}  // namespace

TEST(Insights, RecordAndLoadRoundTrip) {
    auto path = tmp_insights_path();
    {
        InsightsRecorder r(path);
        r.record_model_turn("s1", "anthropic/claude-opus-4-6", 1500, 800,
                            0.012, 2400.0);
        r.record_tool_call("s1", "read_file", 35.5);
        r.record_tool_call("s1", "read_file", 41.0, /*error=*/true);
        r.record_tool_call("s2", "execute_code", 120.0);
    }
    InsightsRecorder r(path);
    auto events = r.load_all();
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0].kind, InsightEvent::Kind::ModelTurn);
    EXPECT_EQ(events[0].input_tokens, 1500);
    EXPECT_EQ(events[1].tool_name, "read_file");
    EXPECT_TRUE(events[2].error);
    std::filesystem::remove_all(path.parent_path());
}

TEST(Insights, SummarizeAggregates) {
    auto path = tmp_insights_path();
    InsightsRecorder r(path);
    r.record_model_turn("a", "m1", 100, 50, 0.001, 1000.0);
    r.record_model_turn("a", "m1", 200, 80, 0.002, 2000.0);
    r.record_model_turn("b", "m2", 300, 90, 0.003, 3000.0);
    r.record_tool_call("a", "read_file", 10.0);
    r.record_tool_call("a", "read_file", 12.0);
    r.record_tool_call("b", "grep", 20.0);

    auto events = r.load_all();
    auto zero = std::chrono::system_clock::time_point{};
    auto sum = InsightsRecorder::summarize(events, zero);
    EXPECT_EQ(sum.sessions, 2);
    EXPECT_EQ(sum.model_turns, 3);
    EXPECT_EQ(sum.tool_calls, 3);
    EXPECT_EQ(sum.input_tokens, 600);
    EXPECT_EQ(sum.output_tokens, 220);
    EXPECT_NEAR(sum.cost_usd, 0.006, 1e-9);
    EXPECT_EQ(sum.tool_call_counts["read_file"], 2);
    EXPECT_EQ(sum.tool_call_counts["grep"], 1);
    EXPECT_EQ(sum.model_turn_counts["m1"], 2);
    EXPECT_GT(sum.latency_p50_ms, 0.0);
    EXPECT_GE(sum.latency_p95_ms, sum.latency_p50_ms);
    std::filesystem::remove_all(path.parent_path());
}

TEST(Insights, CutoffFiltersEvents) {
    auto path = tmp_insights_path();
    InsightsRecorder r(path);
    r.record_model_turn("x", "m", 10, 10, 0.0, 100.0);
    auto events = r.load_all();
    ASSERT_EQ(events.size(), 1u);
    // Cutoff far in the future → nothing counts.
    auto far = std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
    auto sum = InsightsRecorder::summarize(events, far);
    EXPECT_EQ(sum.model_turns, 0);
    EXPECT_EQ(sum.input_tokens, 0);
    std::filesystem::remove_all(path.parent_path());
}

TEST(Insights, RenderContainsKeyFields) {
    InsightSummary s;
    s.sessions = 3;
    s.model_turns = 12;
    s.tool_calls = 40;
    s.input_tokens = 9999;
    s.output_tokens = 4321;
    s.cost_usd = 1.2345;
    s.tool_call_counts = {{"read_file", 25}, {"grep", 15}};
    auto text = s.render();
    EXPECT_NE(text.find("Sessions:"), std::string::npos);
    EXPECT_NE(text.find("9999"), std::string::npos);
    EXPECT_NE(text.find("read_file"), std::string::npos);
    EXPECT_NE(text.find("grep"), std::string::npos);
    EXPECT_NE(text.find("$1.2345"), std::string::npos);
}

TEST(Insights, LoadSkipsBadLines) {
    auto path = tmp_insights_path();
    {
        std::ofstream out(path);
        out << "not a json line\n";
        out << R"({"kind":"tool_call","tool_name":"x","at":"2025-06-01T10:00:00+00:00"})" << '\n';
    }
    InsightsRecorder r(path);
    auto events = r.load_all();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].tool_name, "x");
    std::filesystem::remove_all(path.parent_path());
}

TEST(Insights, SummarizeLastDaysHonoursCutoff) {
    auto path = tmp_insights_path();
    InsightsRecorder r(path);
    r.record_tool_call("s", "read_file", 5.0);
    auto sum = r.summarize_last_days(7);
    EXPECT_EQ(sum.tool_calls, 1);
    std::filesystem::remove_all(path.parent_path());
}
