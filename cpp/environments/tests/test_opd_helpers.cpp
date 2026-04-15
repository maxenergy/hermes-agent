// Tests for environments::opd_helpers — ported from agentic_opd_env.py.

#include <hermes/environments/opd_helpers.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace oh = hermes::environments::opd_helpers;
using nlohmann::json;

TEST(OpdHelpers, JudgeMessagesShape) {
    const auto msgs = oh::build_hint_judge_messages("resp", "tool-output",
                                                      "tool");
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0]["role"], "system");
    EXPECT_FALSE(msgs[0]["content"].get<std::string>().empty());
    EXPECT_EQ(msgs[1]["role"], "user");
    EXPECT_NE(msgs[1]["content"].get<std::string>().find("## Assistant response"),
              std::string::npos);
    EXPECT_NE(msgs[1]["content"].get<std::string>().find("[role: tool]"),
              std::string::npos);
}

TEST(OpdHelpers, ParseHintPositive) {
    const auto r = oh::parse_hint_result(
        "analysis ...\n[HINT_START]add error handling[HINT_END]\n\\boxed{1}");
    ASSERT_TRUE(r.score.has_value());
    EXPECT_EQ(*r.score, 1);
    EXPECT_EQ(r.hint, "add error handling");
}

TEST(OpdHelpers, ParseHintNegative) {
    const auto r = oh::parse_hint_result("no hint\n\\boxed{-1}");
    ASSERT_TRUE(r.score.has_value());
    EXPECT_EQ(*r.score, -1);
    EXPECT_TRUE(r.hint.empty());
}

TEST(OpdHelpers, ParseHintIgnoresOtherScores) {
    const auto r = oh::parse_hint_result("\\boxed{5}");
    EXPECT_FALSE(r.score.has_value());
}

TEST(OpdHelpers, SelectBestHintPicksLongest) {
    const std::vector<oh::HintResult> votes{
        {1, "short"},
        {1, "this is a sufficiently long hint"},
        {1, "another valid hint that is longer still so should win the pick"},
        {-1, "negative"},
    };
    const auto best = oh::select_best_hint(votes);
    ASSERT_TRUE(best.has_value());
    EXPECT_NE(best->hint.find("another valid hint"), std::string::npos);
}

TEST(OpdHelpers, SelectBestHintEmptyVotes) {
    EXPECT_FALSE(oh::select_best_hint({}).has_value());
    const std::vector<oh::HintResult> only_short{{1, "tiny"}};
    EXPECT_FALSE(oh::select_best_hint(only_short).has_value());
}

TEST(OpdHelpers, AppendHintCreatesUserMessageWhenEmpty) {
    const auto out = oh::append_hint_to_messages({}, " be careful ");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0]["role"], "user");
    EXPECT_NE(out[0]["content"].get<std::string>().find("be careful"),
              std::string::npos);
}

TEST(OpdHelpers, AppendHintToLastUserMessage) {
    std::vector<json> messages{
        json{{"role", "system"}, {"content", "sys"}},
        json{{"role", "user"}, {"content", "hello"}},
        json{{"role", "assistant"}, {"content", "hi"}},
        json{{"role", "user"}, {"content", "please help"}},
    };
    const auto out = oh::append_hint_to_messages(messages, "try X");
    EXPECT_EQ(out.size(), messages.size());
    // System / assistant untouched.
    EXPECT_EQ(out[0], messages[0]);
    EXPECT_EQ(out[2], messages[2]);
    EXPECT_NE(out[3]["content"].get<std::string>().find("please help"),
              std::string::npos);
    EXPECT_NE(out[3]["content"].get<std::string>().find("try X"),
              std::string::npos);
    EXPECT_EQ(out[1]["content"], "hello");  // earlier user unchanged
}

TEST(OpdHelpers, AppendHintFlattensListContent) {
    std::vector<json> messages{
        json{{"role", "user"},
              {"content", json::array({json{{"type", "text"}, {"text", "part1"}},
                                          json{{"type", "text"}, {"text", "part2"}}})}},
    };
    const auto out = oh::append_hint_to_messages(messages, "add more detail");
    ASSERT_EQ(out.size(), 1u);
    const auto text = out[0]["content"].get<std::string>();
    EXPECT_NE(text.find("part1"), std::string::npos);
    EXPECT_NE(text.find("part2"), std::string::npos);
    EXPECT_NE(text.find("add more detail"), std::string::npos);
}

TEST(OpdHelpers, ExtractTurnPairsToolResults) {
    std::vector<json> messages{
        json{{"role", "system"}, {"content", "sys"}},
        json{{"role", "user"}, {"content", "do X"}},
        json{{"role", "assistant"}, {"content", "here goes"}},
        json{{"role", "tool"}, {"content", "tool out 1"}},
        json{{"role", "tool"}, {"content", "tool out 2"}},
        json{{"role", "assistant"}, {"content", "final"}},
    };
    const auto pairs = oh::extract_turn_pairs(messages);
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].assistant_text, "here goes");
    EXPECT_EQ(pairs[0].next_state_role, "tool");
    EXPECT_NE(pairs[0].next_state_text.find("tool out 1"), std::string::npos);
    EXPECT_NE(pairs[0].next_state_text.find("tool out 2"), std::string::npos);
    EXPECT_NE(pairs[0].next_state_text.find("\n---\n"), std::string::npos);
    EXPECT_EQ(pairs[0].context_messages.size(), 2u);
}

TEST(OpdHelpers, ExtractTurnPairsUserReply) {
    std::vector<json> messages{
        json{{"role", "assistant"}, {"content", "hi"}},
        json{{"role", "user"}, {"content", "thanks"}},
    };
    const auto pairs = oh::extract_turn_pairs(messages);
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].next_state_role, "user");
    EXPECT_EQ(pairs[0].next_state_text, "thanks");
}

TEST(OpdHelpers, ExtractTurnPairsTruncatesLongToolOutput) {
    std::string big(10'000, 'x');
    std::vector<json> messages{
        json{{"role", "assistant"}, {"content", "go"}},
        json{{"role", "tool"}, {"content", big}},
    };
    const auto pairs = oh::extract_turn_pairs(messages, 200);
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_LE(pairs[0].next_state_text.size(), 200u + 32u);
    EXPECT_NE(pairs[0].next_state_text.find("[truncated]"), std::string::npos);
}

TEST(OpdHelpers, FindTokenSpan) {
    std::vector<int> full{1, 2, 3, 4, 5, 6, 3, 4, 5};
    std::vector<int> sub{3, 4, 5};
    auto r = oh::find_token_span(full, sub);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 6u);  // last occurrence

    r = oh::find_token_span(full, {7, 8});
    EXPECT_FALSE(r.has_value());

    r = oh::find_token_span({}, {1});
    EXPECT_FALSE(r.has_value());
}
