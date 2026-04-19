// Tests for hermes::agent::strip_think_blocks.
//
// Mirrors tests/run_agent/test_run_agent.py::TestStripThinkBlocks from
// upstream commits 9489d157 (unterminated-tag handling) and ec48ec55
// (stored-content strip).
#include "hermes/agent/think_stripper.hpp"

#include <gtest/gtest.h>

using hermes::agent::strip_think_blocks;

TEST(StripThinkBlocks, EmptyPassthrough) {
    EXPECT_EQ(strip_think_blocks(""), "");
}

TEST(StripThinkBlocks, NoTagsPassthrough) {
    EXPECT_EQ(strip_think_blocks("plain assistant response"),
              "plain assistant response");
}

TEST(StripThinkBlocks, ClosedPair) {
    EXPECT_EQ(strip_think_blocks("pre <think>hidden</think> post"),
              "pre  post");
}

TEST(StripThinkBlocks, ClosedPairMultiline) {
    // DOTALL equivalent — inner newlines are consumed.
    const std::string in =
        "visible\n<think>line one\nline two</think>\ntrailing";
    EXPECT_EQ(strip_think_blocks(in), "visible\n\ntrailing");
}

TEST(StripThinkBlocks, MultipleClosedBlocksSameLine) {
    EXPECT_EQ(strip_think_blocks("<think>a</think>X<think>b</think>Y"), "XY");
}

TEST(StripThinkBlocks, MixedCaseClosedPair) {
    // Case-insensitive — ec48ec55 made all variants consistently icase so
    // <THINK>…</THINK> does not slip through to the unterminated-tag pass.
    EXPECT_EQ(strip_think_blocks("before <THINK>x</THINK> after"),
              "before  after");
    EXPECT_EQ(strip_think_blocks("<Thinking>y</Thinking>tail"), "tail");
}

TEST(StripThinkBlocks, UnterminatedAtStart) {
    // NIM / MiniMax drops closing </think>; everything from the open tag
    // to end of string is stripped.
    EXPECT_EQ(strip_think_blocks("<think>orphan reasoning never closed"),
              "");
}

TEST(StripThinkBlocks, UnterminatedAfterNewline) {
    // Tag at a block boundary (after newline) — the leading newline is
    // consumed by the `(?:^|\n)` branch.
    EXPECT_EQ(strip_think_blocks("visible prefix\n<think>leaked reasoning"),
              "visible prefix");
}

TEST(StripThinkBlocks, UnterminatedWithIndent) {
    EXPECT_EQ(
        strip_think_blocks("visible\n\t<thinking>indented orphan"),
        "visible");
}

TEST(StripThinkBlocks, UnterminatedThoughtVariant) {
    EXPECT_EQ(strip_think_blocks("<thought>gemma orphan block\nmore"),
              "");
}

TEST(StripThinkBlocks, ProseMentionNotStripped) {
    // A ``<think>`` mention mid-sentence (not at a block boundary) must
    // NOT trigger the unterminated-tag branch. This mirrors the Python
    // regression test for #10408.
    const std::string in =
        "Use the <think> tag when you want hidden reasoning.";
    // The orphan-tag pass (#3) still removes the bare `<think>` token,
    // but surrounding prose is preserved — matching Python behaviour.
    EXPECT_EQ(strip_think_blocks(in),
              "Use the tag when you want hidden reasoning.");
}

TEST(StripThinkBlocks, CJKContentPreserved) {
    // Visible CJK content surrounding a closed think block must survive
    // the strip byte-for-byte (UTF-8 round-trip).
    const std::string in =
        "你好<think>隐藏推理</think>世界";
    EXPECT_EQ(strip_think_blocks(in), "你好世界");
}

TEST(StripThinkBlocks, CJKWithEmojiPreserved) {
    const std::string in = "答案是:<think>思考</think>42 🎉";
    EXPECT_EQ(strip_think_blocks(in), "答案是:42 🎉");
}

TEST(StripThinkBlocks, ReasoningScratchpadVariant) {
    EXPECT_EQ(
        strip_think_blocks(
            "ok <REASONING_SCRATCHPAD>note</REASONING_SCRATCHPAD> done"),
        "ok  done");
}

TEST(StripThinkBlocks, ReasoningScratchpadUnterminated) {
    EXPECT_EQ(strip_think_blocks("<REASONING_SCRATCHPAD>oops no close"),
              "");
}

TEST(StripThinkBlocks, OrphanCloseTag) {
    // A stray </think> with no matching open is removed by pass #3.
    EXPECT_EQ(strip_think_blocks("final </think>answer"), "final answer");
}
