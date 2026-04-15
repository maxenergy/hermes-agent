// Tests for environments::web_research_helpers — ported from
// environments/web_research_env.py.

#include <hermes/environments/web_research_helpers.hpp>

#include <cmath>

#include <gtest/gtest.h>

namespace wr = hermes::environments::web_research_helpers;

TEST(WebResearchHelpers, ParseJudgeJsonPrimary) {
    const auto r = wr::parse_judge_json(R"({"score": 0.7, "reason": "ok"})");
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.7, 1e-9);
}

TEST(WebResearchHelpers, ParseJudgeJsonFencedBlock) {
    const auto r = wr::parse_judge_json("```json\n{\"score\": 0.9}\n```\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.9, 1e-9);
}

TEST(WebResearchHelpers, ParseJudgeJsonFallbackRegex) {
    // Unbalanced JSON — fallback should find the regex score.
    const auto r = wr::parse_judge_json(R"(Model says "score": 0.42 !! done)");
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.42, 1e-9);
}

TEST(WebResearchHelpers, ParseJudgeJsonRejectsOutOfRange) {
    EXPECT_FALSE(wr::parse_judge_json(R"({"score": 1.5})").has_value());
    EXPECT_FALSE(wr::parse_judge_json(R"({"score": -1})").has_value());
    EXPECT_FALSE(wr::parse_judge_json("no score here").has_value());
}

TEST(WebResearchHelpers, TokenizeDropsStopwordsAndShort) {
    const auto toks = wr::tokenize("The quick brown fox and a cat");
    EXPECT_EQ(toks.count("quick"), 1u);
    EXPECT_EQ(toks.count("brown"), 1u);
    EXPECT_EQ(toks.count("fox"), 1u);
    EXPECT_EQ(toks.count("cat"), 1u);
    EXPECT_EQ(toks.count("the"), 0u);
    EXPECT_EQ(toks.count("and"), 0u);
    EXPECT_EQ(toks.count("a"), 0u);
}

TEST(WebResearchHelpers, HeuristicScoreFullOverlap) {
    const auto s = wr::heuristic_score("hello world beautiful",
                                         "hello world beautiful");
    EXPECT_NEAR(s, 1.0, 1e-9);
}

TEST(WebResearchHelpers, HeuristicScoreNoOverlap) {
    const auto s = wr::heuristic_score("hello world", "foo bar baz");
    EXPECT_NEAR(s, 0.0, 1e-9);
}

TEST(WebResearchHelpers, HeuristicScoreEmptyExpectedFallback) {
    // When the expected answer has no non-trivial tokens, the helper
    // returns the 0.5 "neutral" score.
    const auto s = wr::heuristic_score("a the of", "unrelated answer");
    EXPECT_NEAR(s, 0.5, 1e-9);
}

TEST(WebResearchHelpers, ExtractUrlsDeduplicatesPreservingOrder) {
    // Use whitespace as the sole terminator so we don't have to worry
    // about commas/periods/parens being part of the matched URL — that
    // sloppy greediness is inherited from the Python regex and is
    // intentional: the environment only uses domain extraction where
    // trailing punctuation is stripped later.
    const std::string text =
        "See https://a.example/foo\nand https://b.example/bar\n"
        "also https://a.example/foo again.";
    const auto urls = wr::extract_urls(text);
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "https://a.example/foo");
    EXPECT_EQ(urls[1], "https://b.example/bar");
}

TEST(WebResearchHelpers, ExtractDomainsStripsWww) {
    const std::string text =
        "see https://www.example.com/a and http://www.example.com/b and "
        "https://other.net/x";
    const auto domains = wr::extract_domains(text);
    EXPECT_EQ(domains.count("example.com"), 1u);
    EXPECT_EQ(domains.count("other.net"), 1u);
}

TEST(WebResearchHelpers, EfficiencyCurve) {
    wr::RewardConfig cfg;
    EXPECT_NEAR(wr::efficiency_score(0, cfg), 1.0, 1e-9);
    EXPECT_NEAR(wr::efficiency_score(5, cfg), 1.0, 1e-9);
    EXPECT_NEAR(wr::efficiency_score(6, cfg), 1.0 - 0.08, 1e-9);
    EXPECT_NEAR(wr::efficiency_score(10, cfg), 1.0 - 5 * 0.08, 1e-9);
    EXPECT_NEAR(wr::efficiency_score(11, cfg), 1.0 - 6 * 0.12, 1e-9);
    EXPECT_NEAR(wr::efficiency_score(100, cfg), 0.0, 1e-9);
}

TEST(WebResearchHelpers, UsedWebTool) {
    EXPECT_TRUE(wr::used_web_tool({"web_search"}));
    EXPECT_TRUE(wr::used_web_tool({"file_read", "search"}));
    EXPECT_FALSE(wr::used_web_tool({"file_read", "terminal"}));
    EXPECT_FALSE(wr::used_web_tool({}));
}

TEST(WebResearchHelpers, DiversityAtLeastTwo) {
    wr::RewardConfig cfg;
    cfg.diversity_bonus = 0.1;
    std::unordered_set<std::string> one{"a.com"};
    std::unordered_set<std::string> two{"a.com", "b.com"};
    EXPECT_NEAR(wr::diversity_score(one, cfg), 0.0, 1e-9);
    EXPECT_NEAR(wr::diversity_score(two, cfg), 0.1, 1e-9);
}

TEST(WebResearchHelpers, CombineRewardClamps) {
    wr::RewardConfig cfg;
    // 0.6 * 1 + 0.2 * 1 + 0.2 * 1 + 0.1 = 1.1 → clamped to 1.0.
    EXPECT_NEAR(wr::combine_reward(1.0, 1.0, 1.0, 0.1, cfg), 1.0, 1e-9);
    // 0.6 * 0 + 0.2 * 0 + 0.2 * 0 + 0 = 0.
    EXPECT_NEAR(wr::combine_reward(0.0, 0.0, 0.0, 0.0, cfg), 0.0, 1e-9);
    // 0.6 * 0.5 + 0.2 * 1 + 0.2 * 0.5 + 0 = 0.6
    EXPECT_NEAR(wr::combine_reward(0.5, 1.0, 0.5, 0.0, cfg), 0.6, 1e-9);
}

TEST(WebResearchHelpers, CombineRewardNegativeInputClamped) {
    wr::RewardConfig cfg;
    // Using impossible negative tool-used — combine should still clamp
    // the output at zero rather than emitting a negative reward.
    const auto r = wr::combine_reward(-10.0, -10.0, -10.0, 0.0, cfg);
    EXPECT_NEAR(r, 0.0, 1e-9);
}
