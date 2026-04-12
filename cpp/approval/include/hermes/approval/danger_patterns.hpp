// Canonical list of dangerous shell command regex patterns.
//
// Ported from tools/approval.py DANGEROUS_PATTERNS. Each pattern carries
// a stable id, ECMAScript regex (case-insensitive), category, severity,
// and a human-readable description.
//
// Phase 6 of the C++17 backend port — see plans/cpp17-backend-port.md.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::approval {

struct DangerPattern {
    std::string key;          // short stable id, e.g. "rm_root"
    std::string regex;        // ECMAScript regex (case-insensitive, dotall)
    std::string category;     // filesystem|network|system|database|shell
    std::string description;  // human-readable summary
    int severity = 3;         // 1=low, 2=medium, 3=high
};

// Canonical danger pattern table. At least 45 patterns, deterministic order.
const std::vector<DangerPattern>& danger_patterns();

// Look up a pattern by stable key.
std::optional<DangerPattern> find_pattern(std::string_view key);

}  // namespace hermes::approval
