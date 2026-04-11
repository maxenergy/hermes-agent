#include "hermes/core/ansi_strip.hpp"

#include <gtest/gtest.h>
#include <string>

namespace has_ = hermes::core::ansi_strip;

TEST(AnsiStrip, StripsColorCsi) {
    const std::string input = "\x1B[31mred\x1B[0m text";
    EXPECT_EQ(has_::strip_ansi(input), "red text");
}

TEST(AnsiStrip, StripsMultiParamCsi) {
    const std::string input = "before\x1B[1;32;48;5;208mhighlight\x1B[0mafter";
    EXPECT_EQ(has_::strip_ansi(input), "beforehighlightafter");
}

TEST(AnsiStrip, StripsOscWithBel) {
    const std::string input = "pre\x1B]0;window title\x07post";
    EXPECT_EQ(has_::strip_ansi(input), "prepost");
}

TEST(AnsiStrip, StripsOscWithStringTerminator) {
    const std::string input = "pre\x1B]1337;ArbitraryData\x1B\\post";
    EXPECT_EQ(has_::strip_ansi(input), "prepost");
}

TEST(AnsiStrip, StripsC1Bytes) {
    // 0x9B is 8-bit CSI; it consumes params through the final byte `m`.
    // The subsequent literal text "red" is preserved, then the 7-bit
    // `\x1B[0m` reset is stripped, then `b` survives.
    const std::string input = std::string("a") + "\x9B" + "31mred\x1B[0mb";
    EXPECT_EQ(has_::strip_ansi(input), "aredb");

    // A bare 8-bit control byte (not CSI) like 0x84 should just be dropped.
    const std::string bare = std::string("a") + "\x84" + "b";
    EXPECT_EQ(has_::strip_ansi(bare), "ab");
}

TEST(AnsiStrip, LeavesPlainTextAlone) {
    EXPECT_EQ(has_::strip_ansi("hello\nworld\tok"), "hello\nworld\tok");
    EXPECT_EQ(has_::strip_ansi(""), "");
}
