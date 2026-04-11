#include "hermes/core/time.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <regex>
#include <string>

namespace ht = hermes::core::time;

TEST(Time, NowIsClose) {
    const auto a = std::chrono::system_clock::now();
    const auto b = ht::now();
    const auto diff = std::chrono::duration_cast<std::chrono::seconds>(b - a).count();
    EXPECT_LE(std::abs(diff), 2);
}

TEST(Time, FormatIso8601Shape) {
    const auto now = ht::now();
    const auto formatted = ht::format_iso8601(now);
    // Expect YYYY-MM-DDTHH:MM:SS[+-]HH:MM
    static const std::regex shape(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$)");
    EXPECT_TRUE(std::regex_match(formatted, shape))
        << "formatted timestamp did not match: " << formatted;
}

TEST(Time, FormatIso8601OfEpoch) {
    const auto epoch = std::chrono::system_clock::time_point{};
    const auto formatted = ht::format_iso8601(epoch);
    EXPECT_FALSE(formatted.empty());
    EXPECT_NE(formatted.find('T'), std::string::npos);
}

TEST(Time, ResolvedTimezoneNonCrashing) {
    // Just exercise the lookup path.
    (void)ht::resolved_timezone();
    SUCCEED();
}
