// Formatting helpers mirrored from agent/insights.py / agent/usage_pricing.py.
//
// Keeps the pricing/DB work out of the agent library (that lives in the
// hermes_state / provider adapters) and only ports the pure-format
// utilities: duration rendering, comma-separated integers, session-id
// truncation for the "top sessions" panel, and ASCII bar-chart rendering
// for the "By weekday" / "By hour" overview.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hermes::agent::insights_format {

// Human-friendly duration: "45s", "2m 30s", "3h", "1h 15m", "2d 4h".
// Mirrors agent/usage_pricing.py:format_duration_compact.
std::string format_duration(double seconds);

// Wider render variant used by the terminal report ("Active time: ~N").
// Adds days/hours suffix words when > 1 day.
std::string format_duration_long(double seconds);

// "1,234,567"-style formatting for token counts and cost rows.
std::string format_with_commas(long long n);

// Truncate a session id to 16 chars (matches Python's "s['id'][:16]").
std::string short_session_id(const std::string& id);

// Render a horizontal ASCII bar chart with `width` cells where each
// value is scaled against `max_value` (default: auto = max of values).
// Produces one line per label. Used for by_day / by_hour breakdowns.
struct BarRow {
    std::string label;
    long long value = 0;
};
std::string render_bar_chart(const std::vector<BarRow>& rows,
                             int width = 30,
                             long long max_value = -1);

// Human-friendly count: 7_999_856 → "8.0M", 33_599 → "33.6K", 799 → "799".
// (Duplicated from rate_limit_tracker; kept here for use in insights.)
std::string format_count_short(long long n);

// Pretty-print a UTC epoch seconds value as "Mar 14, 2026" (long) or
// "Mar 14" (short). Empty input → empty string.
std::string format_date_long(std::int64_t epoch_seconds);
std::string format_date_short(std::int64_t epoch_seconds);

}  // namespace hermes::agent::insights_format
