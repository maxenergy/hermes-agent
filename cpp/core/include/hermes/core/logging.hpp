// Phase-0 logging shim: prints to stderr with a timestamp + level
// prefix. Phase 1 will replace this with a spdlog-backed implementation.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace hermes::core::logging {

enum class Level { Debug, Info, Warn, Error };

// Initialise logging for the process. Phase 0 no-op — see TODO.
void setup_logging(const std::filesystem::path& home, const std::string& level);

void log_debug(const std::string& msg);
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Lower-level API used by the tagged helpers below and by tests.
void log(Level level, std::string_view msg);

// Convert a level name (case-insensitive) to an enum. Unknown strings
// map to Info.
Level level_from_string(std::string_view name);

}  // namespace hermes::core::logging
