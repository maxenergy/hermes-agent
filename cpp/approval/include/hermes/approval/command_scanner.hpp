// CommandScanner — normalize a shell command string then test it against
// the canonical danger pattern table.
//
// Normalization pipeline:
//   1. strip ANSI escape sequences (hermes::core::ansi_strip::strip_ansi)
//   2. drop null bytes
//   3. fold ASCII fullwidth latin (U+FF01..U+FF5E) to plain ASCII
//      (lightweight NFKC fallback — no ICU dependency)
//   4. collapse runs of whitespace to a single space
//   5. lowercase
//
// scan() returns ALL matches; a single command can trip multiple patterns.
#pragma once

#include "hermes/approval/danger_patterns.hpp"

#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::approval {

struct Match {
    std::string pattern_key;
    std::string matched_text;
    int severity = 0;
    std::string category;
    std::string description;
};

class CommandScanner {
public:
    CommandScanner();

    std::vector<Match> scan(std::string_view command) const;
    bool is_dangerous(std::string_view command) const;

    // Number of patterns successfully compiled. (Tests can assert this is
    // equal to danger_patterns().size().)
    std::size_t pattern_count() const noexcept { return compiled_.size(); }

private:
    std::vector<std::pair<DangerPattern, std::regex>> compiled_;
};

// Normalize a shell command string per the pipeline above. Exposed so the
// pipeline can be unit tested directly.
std::string normalize_command(std::string_view command);

}  // namespace hermes::approval
