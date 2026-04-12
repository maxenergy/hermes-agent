#include <hermes/cron/cron_parser.hpp>

#include <algorithm>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hermes::cron {
namespace {

std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> parts;
    std::string token;
    for (char c : s) {
        if (c == delim) {
            if (!token.empty()) parts.push_back(std::move(token));
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) parts.push_back(std::move(token));
    return parts;
}

std::set<int> parse_field(std::string_view field, int lo, int hi) {
    std::set<int> result;
    auto parts = split(field, ',');
    if (parts.empty()) {
        throw std::invalid_argument(
            std::string("empty cron field: ") + std::string(field));
    }
    for (const auto& part : parts) {
        // Check for step: */N or N-M/S
        std::string base;
        int step = 1;
        auto slash = part.find('/');
        if (slash != std::string::npos) {
            base = part.substr(0, slash);
            auto step_str = part.substr(slash + 1);
            if (step_str.empty()) {
                throw std::invalid_argument("empty step in cron field");
            }
            step = std::stoi(step_str);
            if (step <= 0) {
                throw std::invalid_argument("step must be positive");
            }
        } else {
            base = part;
        }

        if (base == "*") {
            for (int i = lo; i <= hi; i += step) {
                result.insert(i);
            }
        } else if (base.find('-') != std::string::npos) {
            auto dash = base.find('-');
            int range_lo = std::stoi(base.substr(0, dash));
            int range_hi = std::stoi(base.substr(dash + 1));
            if (range_lo < lo || range_hi > hi || range_lo > range_hi) {
                throw std::invalid_argument("range out of bounds in cron field");
            }
            for (int i = range_lo; i <= range_hi; i += step) {
                result.insert(i);
            }
        } else {
            int val = std::stoi(base);
            if (val < lo || val > hi) {
                throw std::invalid_argument(
                    "value out of range in cron field: " + base);
            }
            if (slash != std::string::npos) {
                // e.g. 5/10 means starting at 5, every 10
                for (int i = val; i <= hi; i += step) {
                    result.insert(i);
                }
            } else {
                result.insert(val);
            }
        }
    }
    return result;
}

std::chrono::system_clock::time_point from_utc_tm(const std::tm& tm) {
    // Make a mutable copy for timegm.
    std::tm copy = tm;
    auto tt = timegm(&copy);
    return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace

CronExpression parse(std::string_view expr) {
    auto fields = split(expr, ' ');
    // Filter empty tokens (extra spaces).
    fields.erase(
        std::remove_if(fields.begin(), fields.end(),
                       [](const std::string& s) { return s.empty(); }),
        fields.end());
    if (fields.size() != 5) {
        throw std::invalid_argument(
            "cron expression must have exactly 5 fields, got " +
            std::to_string(fields.size()));
    }
    CronExpression ce;
    ce.minutes = parse_field(fields[0], 0, 59);
    ce.hours = parse_field(fields[1], 0, 23);
    ce.days_of_month = parse_field(fields[2], 1, 31);
    ce.months = parse_field(fields[3], 1, 12);
    ce.days_of_week = parse_field(fields[4], 0, 6);
    return ce;
}

std::chrono::system_clock::time_point next_fire(
    const CronExpression& expr,
    std::chrono::system_clock::time_point after) {
    // Start from the next minute after `after`.
    auto tt = std::chrono::system_clock::to_time_t(after);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    // Advance to next minute.
    tm.tm_sec = 0;
    tm.tm_min += 1;
    // Normalize.
    auto normalized = timegm(&tm);
    gmtime_r(&normalized, &tm);

    // Iterate at most ~4 years of minutes (safety bound).
    constexpr int MAX_ITER = 366 * 24 * 60 * 4;
    for (int i = 0; i < MAX_ITER; ++i) {
        int mon = tm.tm_mon + 1;    // 1-12
        int dom = tm.tm_mday;       // 1-31
        int dow = tm.tm_wday;       // 0-6 (0=Sunday)
        int hour = tm.tm_hour;      // 0-23
        int min = tm.tm_min;        // 0-59

        if (expr.months.count(mon) == 0) {
            // Advance to first day of next month.
            tm.tm_mday = 1;
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_mon += 1;
            auto t = timegm(&tm);
            gmtime_r(&t, &tm);
            continue;
        }
        if (expr.days_of_month.count(dom) == 0 ||
            expr.days_of_week.count(dow) == 0) {
            // Advance to next day.
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_mday += 1;
            auto t = timegm(&tm);
            gmtime_r(&t, &tm);
            continue;
        }
        if (expr.hours.count(hour) == 0) {
            // Advance to next hour.
            tm.tm_min = 0;
            tm.tm_hour += 1;
            auto t = timegm(&tm);
            gmtime_r(&t, &tm);
            continue;
        }
        if (expr.minutes.count(min) == 0) {
            tm.tm_min += 1;
            auto t = timegm(&tm);
            gmtime_r(&t, &tm);
            continue;
        }
        // All fields match.
        return from_utc_tm(tm);
    }
    // Should not happen for valid expressions.
    throw std::invalid_argument("could not find next fire time within 4 years");
}

std::string describe(const CronExpression& expr) {
    // Simple heuristics for common patterns.
    bool all_min = (expr.minutes.size() == 60);
    bool all_hr = (expr.hours.size() == 24);
    bool all_dom = (expr.days_of_month.size() == 31);
    bool all_mon = (expr.months.size() == 12);
    bool all_dow = (expr.days_of_week.size() == 7);

    // "every minute"
    if (all_min && all_hr && all_dom && all_mon && all_dow) {
        return "every minute";
    }

    // "every N minutes" — check if minutes are evenly spaced from 0
    if (all_hr && all_dom && all_mon && all_dow && expr.minutes.size() > 1) {
        auto it = expr.minutes.begin();
        int first = *it;
        ++it;
        int second = *it;
        int step = second - first;
        if (first == 0 && step > 0) {
            bool even = true;
            int expected = 0;
            for (int v : expr.minutes) {
                if (v != expected) { even = false; break; }
                expected += step;
            }
            if (even) {
                return "every " + std::to_string(step) + " minutes";
            }
        }
    }

    // Single minute + single hour => "daily at HH:MM" or more specific
    if (expr.minutes.size() == 1 && expr.hours.size() == 1 && all_dom &&
        all_mon && all_dow) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", *expr.hours.begin(),
                 *expr.minutes.begin());
        return std::string("daily at ") + buf;
    }

    // Weekdays at specific time
    if (expr.minutes.size() == 1 && expr.hours.size() == 1 && all_dom &&
        all_mon) {
        // Check for Mon-Fri (1-5)
        if (expr.days_of_week == std::set<int>{1, 2, 3, 4, 5}) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", *expr.hours.begin(),
                     *expr.minutes.begin());
            return std::string("weekdays at ") + buf;
        }
    }

    // Monthly
    if (expr.minutes.size() == 1 && expr.hours.size() == 1 &&
        expr.days_of_month.size() == 1 && all_mon && all_dow) {
        return "monthly on day " +
               std::to_string(*expr.days_of_month.begin());
    }

    // Fallback: cron-like summary.
    return "custom schedule";
}

}  // namespace hermes::cron
