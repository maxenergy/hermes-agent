#include <hermes/cron/cron_parser.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <stdexcept>

using namespace hermes::cron;
using Clock = std::chrono::system_clock;

namespace {

Clock::time_point make_utc(int year, int mon, int day, int hour, int min) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = 0;
    return Clock::from_time_t(timegm(&tm));
}

std::tm to_utc(Clock::time_point tp) {
    auto tt = Clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return tm;
}

}  // namespace

TEST(CronParser, EveryFiveMinutes) {
    auto expr = parse("*/5 * * * *");
    EXPECT_EQ(expr.minutes.size(), 12u);
    EXPECT_TRUE(expr.minutes.count(0));
    EXPECT_TRUE(expr.minutes.count(5));
    EXPECT_TRUE(expr.minutes.count(55));
    EXPECT_FALSE(expr.minutes.count(3));
}

TEST(CronParser, WeekdaysAt9am) {
    auto expr = parse("0 9 * * 1-5");
    EXPECT_EQ(expr.minutes, std::set<int>{0});
    EXPECT_EQ(expr.hours, std::set<int>{9});
    EXPECT_EQ(expr.days_of_week, (std::set<int>{1, 2, 3, 4, 5}));
    EXPECT_EQ(expr.days_of_month.size(), 31u);
    EXPECT_EQ(expr.months.size(), 12u);
}

TEST(CronParser, MonthlyFirstDay) {
    auto expr = parse("0 0 1 * *");
    EXPECT_EQ(expr.minutes, std::set<int>{0});
    EXPECT_EQ(expr.hours, std::set<int>{0});
    EXPECT_EQ(expr.days_of_month, std::set<int>{1});
}

TEST(CronParser, NextFireEvery5Min) {
    auto expr = parse("*/5 * * * *");
    auto base = make_utc(2026, 1, 15, 10, 3);
    auto next = next_fire(expr, base);
    auto tm = to_utc(next);
    EXPECT_EQ(tm.tm_hour, 10);
    EXPECT_EQ(tm.tm_min, 5);
}

TEST(CronParser, CommaList) {
    auto expr = parse("1,15 * * * *");
    EXPECT_EQ(expr.minutes, (std::set<int>{1, 15}));
    EXPECT_EQ(expr.hours.size(), 24u);
}

TEST(CronParser, Range) {
    auto expr = parse("10-20 * * * *");
    EXPECT_EQ(expr.minutes.size(), 11u);
    EXPECT_TRUE(expr.minutes.count(10));
    EXPECT_TRUE(expr.minutes.count(20));
    EXPECT_FALSE(expr.minutes.count(9));
    EXPECT_FALSE(expr.minutes.count(21));
}

TEST(CronParser, StepRange) {
    auto expr = parse("0-59/10 * * * *");
    EXPECT_EQ(expr.minutes, (std::set<int>{0, 10, 20, 30, 40, 50}));
}

TEST(CronParser, InvalidThrows) {
    EXPECT_THROW(parse("* * *"), std::invalid_argument);
    EXPECT_THROW(parse("60 * * * *"), std::invalid_argument);
    EXPECT_THROW(parse("* 25 * * *"), std::invalid_argument);
}

TEST(CronParser, NextFireWeekday) {
    auto expr = parse("0 9 * * 1-5");
    // 2026-01-17 is Saturday.
    auto base = make_utc(2026, 1, 17, 8, 0);
    auto next = next_fire(expr, base);
    auto tm = to_utc(next);
    EXPECT_EQ(tm.tm_wday, 1);  // Monday
    EXPECT_EQ(tm.tm_hour, 9);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_mday, 19);
}

TEST(CronParser, DescribeEvery5Min) {
    auto expr = parse("*/5 * * * *");
    EXPECT_EQ(describe(expr), "every 5 minutes");
}

TEST(CronParser, DescribeDailyAt) {
    auto expr = parse("0 9 * * *");
    EXPECT_EQ(describe(expr), "daily at 09:00");
}

TEST(CronParser, DescribeWeekdays) {
    auto expr = parse("0 9 * * 1-5");
    EXPECT_EQ(describe(expr), "weekdays at 09:00");
}
