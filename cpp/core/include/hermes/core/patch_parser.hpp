// Minimal unified-diff parser — handles the format produced by
// `git diff` and `diff -u`. Tolerant of missing counts (default 1).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hermes::core::patch_parser {

struct Hunk {
    int old_start{0};
    int old_count{0};
    int new_start{0};
    int new_count{0};
    // Raw body lines, each starting with ` `, `+`, or `-`.
    std::vector<std::string> lines;
};

struct FileDiff {
    std::string old_path;
    std::string new_path;
    std::vector<Hunk> hunks;
};

// Parse an entire unified diff payload. Never throws; malformed input
// produces a best-effort result.
std::vector<FileDiff> parse_unified_diff(std::string_view text);

}  // namespace hermes::core::patch_parser
