#include "hermes/approval/session_state.hpp"

#include <gtest/gtest.h>

using hermes::approval::ApprovalDecision;
using hermes::approval::ApprovalRequest;
using hermes::approval::Match;
using hermes::approval::SessionApprovalState;

TEST(SessionState, NoMatchesReturnsApproved) {
    SessionApprovalState state;
    auto decision = state.request_cli_approval(
        "sess1", "echo hello", {}, nullptr);
    EXPECT_EQ(decision, ApprovalDecision::Approved);
}

TEST(SessionState, PermanentAllowlistApprovedWithoutCallback) {
    SessionApprovalState state;
    state.add_permanent_approval("echo.*");

    Match m;
    m.pattern_key = "dangerous_echo";

    bool callback_called = false;
    auto decision = state.request_cli_approval(
        "sess1", "echo hello", {m},
        [&](const ApprovalRequest&) -> ApprovalDecision {
            callback_called = true;
            return ApprovalDecision::Denied;
        });
    EXPECT_EQ(decision, ApprovalDecision::Approved);
    EXPECT_FALSE(callback_called);
}

TEST(SessionState, YoloAutoApproves) {
    SessionApprovalState state;
    state.approve_all_for_session("sess1");
    EXPECT_TRUE(state.is_yolo("sess1"));

    Match m;
    m.pattern_key = "dangerous";

    auto decision = state.request_cli_approval(
        "sess1", "rm -rf /", {m}, nullptr);
    EXPECT_EQ(decision, ApprovalDecision::Approved);
}

TEST(SessionState, TwoSessionsIsolated) {
    SessionApprovalState state;
    state.approve_pattern_for_session("sess1", "pattern_a");

    EXPECT_TRUE(state.is_approved("sess1", "pattern_a"));
    EXPECT_FALSE(state.is_approved("sess2", "pattern_a"));
}

TEST(SessionState, ClearSessionRemovesApprovals) {
    SessionApprovalState state;
    state.approve_all_for_session("sess1");
    EXPECT_TRUE(state.is_yolo("sess1"));

    state.clear_session("sess1");
    EXPECT_FALSE(state.is_yolo("sess1"));
}
