// Tests for insights_format helpers.
#include "hermes/agent/insights_format.hpp"

#include <gtest/gtest.h>

using namespace hermes::agent::insights_format;

TEST(InsightsFormat, DurationShort) {
    EXPECT_EQ(format_duration(45), "45s");
    EXPECT_EQ(format_duration(60), "1m");
    EXPECT_EQ(format_duration(90), "1m 30s");
    EXPECT_EQ(format_duration(3600), "1h");
    EXPECT_EQ(format_duration(3900), "1h 5m");
    EXPECT_EQ(format_duration(90000), "1d 1h");
    EXPECT_EQ(format_duration(-10), "0s");
}

TEST(InsightsFormat, DurationLong) {
    EXPECT_NE(format_duration_long(30).find("seconds"), std::string::npos);
    EXPECT_NE(format_duration_long(120).find("minutes"), std::string::npos);
    EXPECT_NE(format_duration_long(7200).find("hours"), std::string::npos);
    EXPECT_NE(format_duration_long(172800).find("days"), std::string::npos);
}

TEST(InsightsFormat, Commas) {
    EXPECT_EQ(format_with_commas(0), "0");
    EXPECT_EQ(format_with_commas(999), "999");
    EXPECT_EQ(format_with_commas(1000), "1,000");
    EXPECT_EQ(format_with_commas(1234567), "1,234,567");
    EXPECT_EQ(format_with_commas(-42000), "-42,000");
}

TEST(InsightsFormat, ShortSessionId) {
    EXPECT_EQ(short_session_id(""), "");
    EXPECT_EQ(short_session_id("short"), "short");
    EXPECT_EQ(short_session_id("abcdefghijklmnopqrst"), "abcdefghijklmnop");
    EXPECT_EQ(short_session_id("exactlysixteennn")
                  .size(),
              16u);
}

TEST(InsightsFormat, BarChartProducesRows) {
    std::vector<BarRow> rows = {
        {"Mon", 10}, {"Tue", 20}, {"Wed", 5},
    };
    std::string out = render_bar_chart(rows, 10);
    int newlines = std::count(out.begin(), out.end(), '\n');
    EXPECT_EQ(newlines, 2);  // 3 rows, 2 separators
    EXPECT_NE(out.find("Mon"), std::string::npos);
    EXPECT_NE(out.find("Wed"), std::string::npos);
    // Tue has the max value → should contain the most filled cells.
    EXPECT_NE(out.find("█"), std::string::npos);
}

TEST(InsightsFormat, BarChartEmpty) {
    EXPECT_EQ(render_bar_chart({}, 10), "");
}

TEST(InsightsFormat, CountShort) {
    EXPECT_EQ(format_count_short(500), "500");
    EXPECT_EQ(format_count_short(1500), "1.5K");
    EXPECT_EQ(format_count_short(2500000), "2.5M");
}

TEST(InsightsFormat, DatesFormatted) {
    // 1773360000 = 2026-03-13 00:00:00 UTC (hard-coded reference date).
    EXPECT_EQ(format_date_short(1773360000), "Mar 13");
    EXPECT_EQ(format_date_long(1773360000), "Mar 13, 2026");
    EXPECT_EQ(format_date_short(0), "");
    EXPECT_EQ(format_date_long(-1), "");
}
