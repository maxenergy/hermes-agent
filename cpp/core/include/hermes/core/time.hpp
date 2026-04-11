// Time / timezone helpers. Phase 0 uses std::chrono and strftime —
// full IANA zone handling lands in Phase 1 with Howard Hinnant's date lib.
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace hermes::core::time {

// Wall-clock "now". Always returns UTC under the hood.
std::chrono::system_clock::time_point now();

// Format `tp` as an ISO-8601 timestamp in the resolved timezone
// (see `resolved_timezone()`), with a trailing offset like `+08:00`.
std::string format_iso8601(std::chrono::system_clock::time_point tp);

// The effective timezone name. Lookup order:
//   1. $HERMES_TIMEZONE
//   2. /etc/timezone (Linux)
//   3. system default (empty string — caller treats as local time)
// Result is cached in a function-local static.
std::string_view resolved_timezone();

}  // namespace hermes::core::time
