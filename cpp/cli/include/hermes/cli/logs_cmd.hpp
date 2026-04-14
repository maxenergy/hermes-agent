// C++17 port of hermes_cli/logs.py — `hermes logs` command.
//
// Supports tail + follow + level / session / --since filters.  All log
// files live under `<HERMES_HOME>/logs/`.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::logs_cmd {

struct Options {
    std::string log_name = "agent";          // "agent" | "errors" | "gateway"
    int num_lines = 50;
    bool follow = false;
    std::string min_level;                   // DEBUG..CRITICAL or "" for all
    std::string session_substr;
    std::string since_rel;                   // "1h", "30m", "2d", etc.
};

// Parse a relative time like "1h", "30m", "2d".  Returns seconds since
// epoch of the cutoff, or nullopt if unparsable.
std::optional<std::chrono::system_clock::time_point>
parse_since(const std::string& s);

// Extract log level from a line, e.g. "2026-04-05 12:00:00,123 INFO ..."
std::optional<std::string> extract_level(const std::string& line);

// Extract the leading timestamp from a line.
std::optional<std::chrono::system_clock::time_point>
extract_timestamp(const std::string& line);

// Apply the Options filters to a line.
bool line_passes(const std::string& line, const Options& opts,
                 std::optional<std::chrono::system_clock::time_point>
                     since_cutoff);

// Read the last N lines from a file (chunked for large files).
std::vector<std::string> read_last_n_lines(
    const std::filesystem::path& path, int n);

// Return the path for a named log.
std::filesystem::path log_path_for(const std::string& name);

// Dispatch entry.
int run(int argc, char* argv[]);

// List available log files under $HERMES_HOME/logs/.
int cmd_list();

// Tail + optional follow + filter.
int cmd_tail(const Options& opts);

}  // namespace hermes::cli::logs_cmd
