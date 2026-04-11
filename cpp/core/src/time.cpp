#include "hermes/core/time.hpp"

#include "hermes/core/strings.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace hermes::core::time {

namespace {

std::string load_tz_from_disk() {
    std::ifstream in("/etc/timezone");
    if (!in) {
        return {};
    }
    std::string line;
    std::getline(in, line);
    return hermes::core::strings::trim(line);
}

std::string compute_timezone() {
    if (const char* env = std::getenv("HERMES_TIMEZONE"); env != nullptr && *env != '\0') {
        return env;
    }
    auto disk = load_tz_from_disk();
    if (!disk.empty()) {
        return disk;
    }
    if (const char* tz = std::getenv("TZ"); tz != nullptr && *tz != '\0') {
        return tz;
    }
    return {};
}

}  // namespace

std::string_view resolved_timezone() {
    static const std::string cached = compute_timezone();
    return cached;
}

std::chrono::system_clock::time_point now() {
    return std::chrono::system_clock::now();
}

std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    // Respect the resolved zone by injecting TZ into the environment for
    // the duration of this call. std::localtime_r picks it up via tzset().
    // We guard with a static mutex because ::setenv is not thread-safe
    // when multiple threads read the same env table concurrently.
    static std::mutex tz_mutex;
    std::lock_guard<std::mutex> lock(tz_mutex);

    const auto zone = resolved_timezone();
    std::string saved_tz;
    const char* had_tz = std::getenv("TZ");
    const bool needs_override = !zone.empty() && (had_tz == nullptr || zone != had_tz);
    if (needs_override) {
        if (had_tz != nullptr) {
            saved_tz = had_tz;
        }
        ::setenv("TZ", std::string(zone).c_str(), 1);
        ::tzset();
    }

    const auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    ::localtime_r(&tt, &local);

    std::array<char, 32> buf{};
    // ISO-8601 with numeric offset, e.g. 2026-04-12T03:54:00+0800.
    const auto written = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%S%z", &local);

    if (needs_override) {
        if (had_tz != nullptr) {
            ::setenv("TZ", saved_tz.c_str(), 1);
        } else {
            ::unsetenv("TZ");
        }
        ::tzset();
    }

    if (written == 0) {
        return {};
    }
    std::string out(buf.data(), written);
    // Insert the `:` between hour and minute of the offset for ISO-8601
    // compliance (`+0800` -> `+08:00`).
    if (out.size() >= 5) {
        const char sign = out[out.size() - 5];
        if (sign == '+' || sign == '-') {
            out.insert(out.size() - 2, ":");
        }
    }
    return out;
}

}  // namespace hermes::core::time
