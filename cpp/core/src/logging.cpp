#include "hermes/core/logging.hpp"

#include "hermes/core/strings.hpp"
#include "hermes/core/time.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

namespace hermes::core::logging {

namespace {

std::atomic<Level> g_min_level{Level::Info};
std::mutex g_mu;

const char* level_tag(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

}  // namespace

Level level_from_string(std::string_view name) {
    const auto lower = hermes::core::strings::to_lower(name);
    if (lower == "debug") { return Level::Debug; }
    if (lower == "warn" || lower == "warning") { return Level::Warn; }
    if (lower == "error" || lower == "err") { return Level::Error; }
    return Level::Info;
}

void setup_logging(const std::filesystem::path& /*home*/, const std::string& level) {
    g_min_level.store(level_from_string(level), std::memory_order_relaxed);
    // Uses stderr when spdlog is not linked.
}

void log(Level level, std::string_view msg) {
    if (static_cast<int>(level) < static_cast<int>(g_min_level.load(std::memory_order_relaxed))) {
        return;
    }
    const auto ts = hermes::core::time::format_iso8601(hermes::core::time::now());
    std::lock_guard<std::mutex> lock(g_mu);
    std::cerr << ts << " [" << level_tag(level) << "] hermes_core: " << msg << '\n';
}

void log_debug(const std::string& msg) { log(Level::Debug, msg); }
void log_info(const std::string& msg)  { log(Level::Info, msg); }
void log_warn(const std::string& msg)  { log(Level::Warn, msg); }
void log_error(const std::string& msg) { log(Level::Error, msg); }

}  // namespace hermes::core::logging
