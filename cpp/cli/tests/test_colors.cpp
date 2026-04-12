#include "hermes/cli/colors.hpp"

#include <gtest/gtest.h>

namespace cc = hermes::cli::colors;

TEST(Colors, BoldContainsEscapeOrPlain) {
    auto result = cc::bold("test");
    // When running in a TTY, should contain ANSI escape; when not, should
    // be the plain string. Either way the original text must be present.
    EXPECT_NE(result.find("test"), std::string::npos);
}

TEST(Colors, DimContainsText) {
    EXPECT_NE(cc::dim("hello").find("hello"), std::string::npos);
}

TEST(Colors, RedContainsText) {
    EXPECT_NE(cc::red("err").find("err"), std::string::npos);
}

TEST(Colors, GreenContainsText) {
    EXPECT_NE(cc::green("ok").find("ok"), std::string::npos);
}

TEST(Colors, YellowContainsText) {
    EXPECT_NE(cc::yellow("warn").find("warn"), std::string::npos);
}

TEST(Colors, BlueContainsText) {
    EXPECT_NE(cc::blue("info").find("info"), std::string::npos);
}

TEST(Colors, CyanContainsText) {
    EXPECT_NE(cc::cyan("note").find("note"), std::string::npos);
}

TEST(Colors, HexContainsText) {
    EXPECT_NE(cc::hex("color", "FF8800").find("color"), std::string::npos);
}

TEST(Colors, HexInvalidColorReturnPlain) {
    // Invalid hex color (wrong length) should return the string unchanged.
    auto result = cc::hex("plain", "XYZ");
    EXPECT_EQ(result, "plain");
}

TEST(Colors, NonTtyProducesPlainStrings) {
    // When not a TTY (common in CI), functions should return undecorated
    // strings — at minimum the text is preserved.
    if (!cc::is_tty()) {
        EXPECT_EQ(cc::bold("x"), "x");
        EXPECT_EQ(cc::dim("x"), "x");
        EXPECT_EQ(cc::red("x"), "x");
        EXPECT_EQ(cc::green("x"), "x");
    }
}
