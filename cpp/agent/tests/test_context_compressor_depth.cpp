#include "hermes/agent/context_compressor_depth.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::compressor_depth;

namespace {
Message mk_user(const std::string& text) {
    Message m; m.role = "user"; m.content = text; return m;
}
Message mk_asst(const std::string& text) {
    Message m; m.role = "assistant"; m.content = text; return m;
}
Message mk_tool(const std::string& id, const std::string& text) {
    Message m; m.role = "tool"; m.tool_call_id = id; m.content = text; return m;
}
Message mk_asst_tc(const std::string& text,
                   std::initializer_list<ToolCall> tcs) {
    Message m; m.role = "assistant"; m.content = text;
    for (const auto& tc : tcs) m.tool_calls.push_back(tc);
    return m;
}
}  // namespace

TEST(CompressorDepth, EstimateMessageRoughAccounts) {
    Message m = mk_user("abcd");  // 4 chars / 4 + 10 = 11
    EXPECT_EQ(estimate_message_tokens_rough(m), 11);
}

TEST(CompressorDepth, EstimateMessageIncludesToolArgs) {
    Message m = mk_asst_tc("ab",
        { ToolCall{"c1", "x", std::string(40, 'a')} });  // 40/4 = 10
    // content: 2/4 + 10 = 10 ; args: 40/4 = 10 → 20
    EXPECT_EQ(estimate_message_tokens_rough(m), 20);
}

TEST(CompressorDepth, EstimateMessagesSumsAll) {
    std::vector<Message> msgs = {mk_user("abcd"), mk_asst("efgh")};
    EXPECT_EQ(estimate_messages_tokens_rough(msgs), 22);
}

TEST(CompressorDepth, DeriveMaxSummaryTokens) {
    EXPECT_EQ(derive_max_summary_tokens(200000), 10000);
    EXPECT_EQ(derive_max_summary_tokens(400000), 12000);  // hits ceiling
    EXPECT_EQ(derive_max_summary_tokens(100000), 5000);
}

TEST(CompressorDepth, DeriveTailTokenBudget) {
    EXPECT_EQ(derive_tail_token_budget(100000, 0.20), 20000);
    EXPECT_EQ(derive_tail_token_budget(100000, 0.05), 10000);  // clamp → 0.10
    EXPECT_EQ(derive_tail_token_budget(100000, 0.90), 80000);  // clamp → 0.80
    EXPECT_EQ(derive_tail_token_budget(0, 0.20), 0);
}

TEST(CompressorDepth, DeriveThresholdTokens) {
    EXPECT_EQ(derive_threshold_tokens(200000, 0.50), 100000);
    EXPECT_EQ(derive_threshold_tokens(0, 0.50), 0);
}

TEST(CompressorDepth, ClampSummaryTargetRatio) {
    EXPECT_DOUBLE_EQ(clamp_summary_target_ratio(0.05), 0.10);
    EXPECT_DOUBLE_EQ(clamp_summary_target_ratio(0.90), 0.80);
    EXPECT_DOUBLE_EQ(clamp_summary_target_ratio(0.30), 0.30);
}

TEST(CompressorDepth, ComputeSummaryBudgetMinFloor) {
    std::vector<Message> turns = {mk_user("tiny")};
    // content tokens small ⇒ floor at kMinSummaryTokens
    EXPECT_EQ(compute_summary_budget(turns), kMinSummaryTokens);
}

TEST(CompressorDepth, ComputeSummaryBudgetScales) {
    // 100 messages × (400 chars each → 110 tokens) ≈ 11000 tokens
    // × 0.20 = 2200 → clamped via cfg if too high.
    std::vector<Message> turns;
    for (int i = 0; i < 100; ++i) turns.push_back(mk_user(std::string(400, 'x')));
    SummaryBudgetConfig cfg; cfg.max_summary_tokens = 10000;
    auto b = compute_summary_budget(turns, cfg);
    EXPECT_EQ(b, 2200);
}

TEST(CompressorDepth, ComputeSummaryBudgetCeiling) {
    std::vector<Message> turns;
    for (int i = 0; i < 10000; ++i) turns.push_back(mk_user(std::string(800, 'x')));
    SummaryBudgetConfig cfg; cfg.max_summary_tokens = 5000;
    EXPECT_EQ(compute_summary_budget(turns, cfg), 5000);
}

TEST(CompressorDepth, WithSummaryPrefixEmpty) {
    EXPECT_EQ(with_summary_prefix(""), std::string(kSummaryPrefix));
    EXPECT_EQ(with_summary_prefix("   "), std::string(kSummaryPrefix));
}

TEST(CompressorDepth, WithSummaryPrefixStripsLegacy) {
    std::string input = std::string(kLegacySummaryPrefix) + " the body";
    std::string out = with_summary_prefix(input);
    EXPECT_EQ(out, std::string(kSummaryPrefix) + "\nthe body");
}

TEST(CompressorDepth, WithSummaryPrefixStripsNew) {
    std::string input = std::string(kSummaryPrefix) + "\nbody2";
    std::string out = with_summary_prefix(input);
    EXPECT_EQ(out, std::string(kSummaryPrefix) + "\nbody2");
}

TEST(CompressorDepth, WithSummaryPrefixPassesThrough) {
    std::string out = with_summary_prefix("just a body");
    EXPECT_EQ(out, std::string(kSummaryPrefix) + "\njust a body");
}

TEST(CompressorDepth, SanitizeRemovesOrphanResult) {
    std::vector<Message> msgs = {
        mk_user("hi"),
        mk_tool("orphan", "leftover"),
        mk_asst("ok"),
    };
    SanitizeReport rep;
    auto out = sanitize_tool_pairs(msgs, &rep);
    EXPECT_EQ(rep.orphan_results_removed, 1u);
    EXPECT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[1].role, "assistant");
}

TEST(CompressorDepth, SanitizeInsertsStubForMissingResult) {
    std::vector<Message> msgs = {
        mk_user("hi"),
        mk_asst_tc("using tool", { ToolCall{"c1", "fn", "{}"} }),
        mk_user("thanks"),
    };
    SanitizeReport rep;
    auto out = sanitize_tool_pairs(msgs, &rep);
    EXPECT_EQ(rep.stubs_inserted, 1u);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[1].role, "assistant");
    EXPECT_EQ(out[2].role, "tool");
    EXPECT_EQ(out[2].tool_call_id, "c1");
}

TEST(CompressorDepth, SanitizeNoOpOnBalanced) {
    std::vector<Message> msgs = {
        mk_asst_tc("x", { ToolCall{"c1", "fn", "{}"} }),
        mk_tool("c1", "done"),
    };
    SanitizeReport rep;
    auto out = sanitize_tool_pairs(msgs, &rep);
    EXPECT_EQ(rep.orphan_results_removed, 0u);
    EXPECT_EQ(rep.stubs_inserted, 0u);
    EXPECT_EQ(out.size(), 2u);
}

TEST(CompressorDepth, AlignForwardSkipsTool) {
    std::vector<Message> msgs = {mk_tool("c1", "x"), mk_tool("c2", "y"), mk_user("u")};
    EXPECT_EQ(align_boundary_forward(msgs, 0), 2u);
}

TEST(CompressorDepth, AlignBackwardPullsIntoAssistantGroup) {
    std::vector<Message> msgs = {
        mk_user("u"),
        mk_asst_tc("go", { ToolCall{"c1","fn","{}"} }),
        mk_tool("c1","r"),
        mk_user("next"),
    };
    // idx=3 → look at msgs[2]=tool, walk back to msgs[1]=assistant-with-tc
    EXPECT_EQ(align_boundary_backward(msgs, 3), 1u);
}

TEST(CompressorDepth, AlignBackwardNoChangeWhenClean) {
    std::vector<Message> msgs = {mk_user("u"), mk_asst("a"), mk_user("b")};
    EXPECT_EQ(align_boundary_backward(msgs, 2), 2u);
}

TEST(CompressorDepth, FindTailCutProtectsMinimum) {
    std::vector<Message> msgs;
    for (int i = 0; i < 10; ++i) msgs.push_back(mk_user(std::string(100, 'x')));
    // Token budget tiny → still protects at least 3 messages.
    auto cut = find_tail_cut_by_tokens(msgs, /*head_end=*/1, /*budget=*/1);
    EXPECT_LE(cut, 7u);  // cut at or before index 7, tail ≥ 3
    EXPECT_GE(cut, 2u);
}

TEST(CompressorDepth, FindTailCutBudgetSatisfied) {
    std::vector<Message> msgs;
    for (int i = 0; i < 10; ++i) msgs.push_back(mk_user(std::string(40, 'x')));
    // Each msg ≈ 10+10 = 20 tokens; with budget 100, ~7 messages fit
    auto cut = find_tail_cut_by_tokens(msgs, 1, 100);
    EXPECT_LT(cut, 10u);
    EXPECT_GT(cut, 1u);
}

TEST(CompressorDepth, PruneOldToolResultsReplacesLongContent) {
    std::vector<Message> msgs = {
        mk_user("q"),
        mk_tool("c1", std::string(500, 'a')),
        mk_tool("c2", std::string(500, 'b')),
        mk_user("q2"),
        mk_tool("c3", std::string(500, 'c')),
    };
    PruneReport rep;
    auto out = prune_old_tool_results(msgs, /*tail_count=*/1, /*budget=*/0, &rep);
    // With budget=0, branch uses tail_count=1 → prune everything before index 4.
    EXPECT_EQ(rep.pruned, 2u);
    EXPECT_EQ(out[1].content, std::string(kPrunedToolPlaceholder));
    EXPECT_EQ(out[2].content, std::string(kPrunedToolPlaceholder));
    EXPECT_EQ(out[4].content, std::string(500, 'c'));  // tail preserved
}

TEST(CompressorDepth, PruneOldToolSkipsShortContent) {
    std::vector<Message> msgs = {
        mk_user("q"),
        mk_tool("c1", "short"),  // ≤200 chars → not pruned
        mk_user("q2"),
    };
    PruneReport rep;
    auto out = prune_old_tool_results(msgs, 1, 0, &rep);
    EXPECT_EQ(rep.pruned, 0u);
    EXPECT_EQ(out[1].content, "short");
}

TEST(CompressorDepth, SerializeIncludesToolCallsBlock) {
    std::vector<Message> msgs = {
        mk_asst_tc("hello", { ToolCall{"c1", "read_file", R"({"path":"/foo"})"} }),
        mk_tool("c1", "file contents"),
    };
    std::string s = serialize_for_summary(msgs);
    EXPECT_NE(s.find("[ASSISTANT]:"), std::string::npos);
    EXPECT_NE(s.find("[Tool calls:"), std::string::npos);
    EXPECT_NE(s.find("read_file"), std::string::npos);
    EXPECT_NE(s.find("[TOOL RESULT c1]:"), std::string::npos);
}

TEST(CompressorDepth, SerializeTruncatesLargeContent) {
    std::vector<Message> msgs = { mk_user(std::string(10000, 'x')) };
    std::string s = serialize_for_summary(msgs);
    EXPECT_NE(s.find("[truncated]"), std::string::npos);
    EXPECT_LT(s.size(), 10000u);
}

TEST(CompressorDepth, SerializeTruncatesToolArgs) {
    std::vector<Message> msgs = {
        mk_asst_tc("x", { ToolCall{"c1", "fn", std::string(3000, 'y')} }),
    };
    std::string s = serialize_for_summary(msgs);
    EXPECT_NE(s.find("fn("), std::string::npos);
    // 3000 chars of 'y' ≈ truncated at 1200+"..." length
    EXPECT_LT(s.size(), 3000u);
}

TEST(CompressorDepth, PickSummaryRoleHeadAssistantTailAssistant) {
    // head=assistant ⇒ base=user; tail=assistant, no collision.
    auto r = pick_summary_role("assistant", "assistant");
    EXPECT_EQ(r.role, "user");
    EXPECT_FALSE(r.merge_into_tail);
}

TEST(CompressorDepth, PickSummaryRoleHeadUserTailUser) {
    // head=user ⇒ base=assistant; tail=user, no collision.
    auto r = pick_summary_role("user", "user");
    EXPECT_EQ(r.role, "assistant");
    EXPECT_FALSE(r.merge_into_tail);
}

TEST(CompressorDepth, PickSummaryRoleCollisionFlips) {
    // head=tool → base pick is "user"; if tail is also "user", flip to
    // "assistant" — which does not collide with head="tool".
    auto r = pick_summary_role("tool", "user");
    EXPECT_EQ(r.role, "assistant");
    EXPECT_FALSE(r.merge_into_tail);
}

TEST(CompressorDepth, PickSummaryRoleMergesWhenBothCollide) {
    // head=assistant ⇒ base pick user; tail=user ⇒ flip to assistant
    // ⇒ collides with head ⇒ merge_into_tail.
    auto r = pick_summary_role("assistant", "user");
    // Actually this yields role=user but tail=user → flip to assistant,
    // and flipped(assistant) == head(assistant) → merge.
    EXPECT_TRUE(r.merge_into_tail);
}

TEST(CompressorDepth, CanCompress) {
    EXPECT_FALSE(can_compress(0, 3));
    EXPECT_FALSE(can_compress(7, 3));   // need > 3+3+1=7
    EXPECT_TRUE(can_compress(8, 3));
    EXPECT_TRUE(can_compress(100, 3));
}
