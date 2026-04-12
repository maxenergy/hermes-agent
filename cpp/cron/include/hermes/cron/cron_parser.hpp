// Cron expression parser — 5-field standard cron syntax.
// Supports: *, */N, N, N-M, N-M/S, comma-separated lists.
#pragma once

#include <chrono>
#include <set>
#include <string>
#include <string_view>

namespace hermes::cron {

struct CronExpression {
    std::set<int> minutes;        // 0-59
    std::set<int> hours;          // 0-23
    std::set<int> days_of_month;  // 1-31
    std::set<int> months;         // 1-12
    std::set<int> days_of_week;   // 0-6 (0=Sunday)
};

// Parse "*/5 * * * *" style 5-field cron expression.
// Throws std::invalid_argument on parse error.
CronExpression parse(std::string_view expr);

// Given a time point, compute the next fire time (strictly after `after`).
std::chrono::system_clock::time_point next_fire(
    const CronExpression& expr,
    std::chrono::system_clock::time_point after);

// Human-readable description: "every 5 minutes", "daily at 09:00", etc.
std::string describe(const CronExpression& expr);

}  // namespace hermes::cron
