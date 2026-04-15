// Unit tests for hermes::gateway stream_consumer_text helpers — depth port
// of gateway/stream_consumer.py.

#include "hermes/gateway/stream_consumer_text.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace hermes::gateway;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// clean_for_display + has_media_directives
// ---------------------------------------------------------------------------

TEST(StreamConsumerText, HasMediaDirectivesTrue) {
    EXPECT_TRUE(has_media_directives("prefix MEDIA:/tmp/x.png suffix"));
    EXPECT_TRUE(has_media_directives("hi [[audio_as_voice]]"));
}

TEST(StreamConsumerText, HasMediaDirectivesFalse) {
    EXPECT_FALSE(has_media_directives("plain text"));
    EXPECT_FALSE(has_media_directives(""));
}

TEST(StreamConsumerText, CleanForDisplayNoOp) {
    EXPECT_EQ(clean_for_display("hello world"), "hello world");
}

TEST(StreamConsumerText, CleanForDisplayStripsMediaTag) {
    EXPECT_EQ(clean_for_display("Hello MEDIA:/tmp/a.png world"),
              "Hello  world");
}

TEST(StreamConsumerText, CleanForDisplayStripsQuotedMediaTag) {
    EXPECT_EQ(clean_for_display("Hello \"MEDIA:/tmp/a.png\" done"),
              "Hello  done");
}

TEST(StreamConsumerText, CleanForDisplayStripsAudioMarker) {
    EXPECT_EQ(clean_for_display("voice note [[audio_as_voice]] inline"),
              "voice note  inline");
}

TEST(StreamConsumerText, CleanForDisplayCollapsesBlankLines) {
    // Three+ newlines should collapse down to two.
    auto s = std::string("a\n\n\n\nMEDIA:/x.png\n\nb");
    auto got = clean_for_display(s);
    // After stripping the tag we have six consecutive newlines; the
    // collapse pass reduces any run of 3+ down to exactly two.
    EXPECT_EQ(got, "a\n\nb");
}

TEST(StreamConsumerText, CleanForDisplayRightStripsWhitespace) {
    EXPECT_EQ(clean_for_display("text MEDIA:/x \n\n\t  "), "text");
}

// ---------------------------------------------------------------------------
// continuation_text
// ---------------------------------------------------------------------------

TEST(StreamConsumerText, ContinuationReturnsTailWhenPrefixMatches) {
    EXPECT_EQ(continuation_text("hello world and more", "hello world"),
              "and more");
}

TEST(StreamConsumerText, ContinuationReturnsFullWhenPrefixMismatch) {
    EXPECT_EQ(continuation_text("abc", "xyz"), "abc");
}

TEST(StreamConsumerText, ContinuationEmptyPrefixKeepsWholeText) {
    EXPECT_EQ(continuation_text("hello world", ""), "hello world");
}

TEST(StreamConsumerText, ContinuationTrimsLeadingWhitespace) {
    EXPECT_EQ(continuation_text("prefix\n\n  tail", "prefix"), "tail");
}

// ---------------------------------------------------------------------------
// visible_prefix
// ---------------------------------------------------------------------------

TEST(StreamConsumerText, VisiblePrefixStripsCursor) {
    EXPECT_EQ(visible_prefix("hello ▉", " ▉"), "hello");
}

TEST(StreamConsumerText, VisiblePrefixWithNoCursor) {
    EXPECT_EQ(visible_prefix("plain", " ▉"), "plain");
}

TEST(StreamConsumerText, VisiblePrefixAlsoCleansMediaTags) {
    EXPECT_EQ(visible_prefix("text MEDIA:/x  ▉", " ▉"), "text");
}

// ---------------------------------------------------------------------------
// split_text_chunks + safe_split_limit + compute_split_offset
// ---------------------------------------------------------------------------

TEST(StreamConsumerText, SplitReturnsWholeWhenUnderLimit) {
    auto chunks = split_text_chunks("short", 100);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "short");
}

TEST(StreamConsumerText, SplitAtNewlineBoundary) {
    std::string body = std::string(40, 'a') + "\n" + std::string(40, 'b');
    auto chunks = split_text_chunks(body, 50);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], std::string(40, 'a'));
    EXPECT_EQ(chunks[1], std::string(40, 'b'));
}

TEST(StreamConsumerText, SplitFallsBackToHardCutWhenNoGoodBoundary) {
    std::string body(120, 'x');
    auto chunks = split_text_chunks(body, 50);
    // Body has no newline → each chunk is limit-sized up to the tail.
    EXPECT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].size(), 50u);
    EXPECT_EQ(chunks[1].size(), 50u);
    EXPECT_EQ(chunks[2].size(), 20u);
}

TEST(StreamConsumerText, SafeSplitLimitUsesCursorAndBuffer) {
    // 4096 - len(cursor) - 100 = 4096 - 4 - 100 = 3992.
    EXPECT_EQ(safe_split_limit(4096, " ▉"), 4096u - 4u - 100u);
}

TEST(StreamConsumerText, SafeSplitLimitFloorsAt500) {
    EXPECT_EQ(safe_split_limit(100, " ▉"), 500u);
}

TEST(StreamConsumerText, ComputeSplitOffsetNewlineWithinSecondHalf) {
    std::string s = std::string(30, 'a') + "\n" + std::string(30, 'b');
    // limit=50 → rfind '\n' in [0,50] finds pos=30, which is >= 25 → accept.
    EXPECT_EQ(compute_split_offset(s, 50), 30u);
}

TEST(StreamConsumerText, ComputeSplitOffsetFallbackWhenNewlineTooEarly) {
    std::string s = "x\n" + std::string(60, 'a');
    // Newline at pos 1, < limit/2=25 → fallback to safe_limit.
    EXPECT_EQ(compute_split_offset(s, 50), 50u);
}

TEST(StreamConsumerText, ComputeSplitOffsetReturnsSizeWhenShort) {
    EXPECT_EQ(compute_split_offset("abc", 100), 3u);
}

// ---------------------------------------------------------------------------
// should_edit_now + render_intermediate_body
// ---------------------------------------------------------------------------

TEST(StreamConsumerText, ShouldEditWhenDone) {
    StreamConsumerTextConfig cfg;
    EXPECT_TRUE(should_edit_now(true, false, 0, 0.0s, cfg));
}

TEST(StreamConsumerText, ShouldEditWhenSegmentBreak) {
    StreamConsumerTextConfig cfg;
    EXPECT_TRUE(should_edit_now(false, true, 0, 0.0s, cfg));
}

TEST(StreamConsumerText, ShouldEditOnBufferThreshold) {
    StreamConsumerTextConfig cfg;
    EXPECT_TRUE(should_edit_now(false, false, cfg.buffer_threshold, 0.0s, cfg));
}

TEST(StreamConsumerText, ShouldEditOnElapsedInterval) {
    StreamConsumerTextConfig cfg;
    EXPECT_TRUE(should_edit_now(false, false, 5,
                                std::chrono::duration<double>(0.5), cfg));
}

TEST(StreamConsumerText, ShouldNotEditTooSoonWithSmallBuffer) {
    StreamConsumerTextConfig cfg;
    EXPECT_FALSE(should_edit_now(false, false, 5,
                                 std::chrono::duration<double>(0.05), cfg));
}

TEST(StreamConsumerText, ShouldNotEditWithEmptyBuffer) {
    StreamConsumerTextConfig cfg;
    EXPECT_FALSE(should_edit_now(false, false, 0,
                                 std::chrono::duration<double>(5.0), cfg));
}

TEST(StreamConsumerText, RenderIntermediateAppendsCursor) {
    EXPECT_EQ(render_intermediate_body("hello", false, false, " ▉"), "hello ▉");
}

TEST(StreamConsumerText, RenderIntermediateOmitsCursorOnDone) {
    EXPECT_EQ(render_intermediate_body("final", true, false, " ▉"), "final");
}

TEST(StreamConsumerText, RenderIntermediateOmitsCursorOnSegmentBreak) {
    EXPECT_EQ(render_intermediate_body("seg", false, true, " ▉"), "seg");
}
