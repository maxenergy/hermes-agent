// Skills Guard — Security scanner for externally-sourced skills.
//
// Port of tools/skills_guard.py.  Performs regex-based static analysis on a
// skill directory to detect known-bad patterns (data exfiltration, prompt
// injection, destructive commands, persistence, etc.) and a trust-aware
// install policy that determines whether a skill is allowed based on both
// the scan verdict and the source's trust level.
//
// Trust levels:
//   - builtin:       Ships with Hermes. Never scanned, always trusted.
//   - trusted:       openai/skills and anthropics/skills only. Caution allowed.
//   - community:     Everything else. Any findings = blocked unless force.
//   - agent-created: Permissive, ask on dangerous.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::skills::guard {

struct Finding {
    std::string pattern_id;
    std::string severity;   // "critical" | "high" | "medium" | "low"
    std::string category;   // "exfiltration" | "injection" | ...
    std::string file;
    int line = 0;
    std::string match;
    std::string description;
};

struct ScanResult {
    std::string skill_name;
    std::string source;
    std::string trust_level;  // "builtin" | "trusted" | "community" | "agent-created"
    std::string verdict;      // "safe" | "caution" | "dangerous"
    std::vector<Finding> findings;
    std::string scanned_at;   // ISO-8601 UTC
    std::string summary;
};

// Decision returned by should_allow_install — None in Python maps to
// `needs_confirmation` here.
enum class InstallDecision { Allow, Block, NeedsConfirmation };

struct InstallVerdict {
    InstallDecision decision = InstallDecision::Block;
    std::string reason;
};

// Map a source identifier (e.g. "openai/skills", "community",
// "agent-created") to a trust level.
std::string resolve_trust_level(std::string_view source);

// Determine the overall verdict from a list of findings.
std::string determine_verdict(const std::vector<Finding>& findings);

// Scan a single text file for threats.  `rel_path` is the relative path to
// embed in findings (defaults to the file name).
std::vector<Finding> scan_file(const std::filesystem::path& file_path,
                               std::string_view rel_path = {});

// Structural checks: file count, total size, binaries, symlinks, etc.
std::vector<Finding> check_structure(const std::filesystem::path& skill_dir);

// Scan an entire skill directory (recursively).
ScanResult scan_skill(const std::filesystem::path& skill_path,
                      std::string_view source = "community");

// Install policy: given a scan result, should this skill be installed?
// `force` overrides blocked decisions.
InstallVerdict should_allow_install(const ScanResult& result, bool force = false);

// Format a scan result as a human-readable report.
std::string format_scan_report(const ScanResult& result);

// SHA-256 hash of all files in a skill directory, used for provenance
// tracking.  Returns "sha256:<first-16-hex>".
std::string content_hash(const std::filesystem::path& skill_path);

// Convenience: count findings by severity.
struct SeverityCounts {
    int critical = 0;
    int high = 0;
    int medium = 0;
    int low = 0;
};
SeverityCounts count_severities(const std::vector<Finding>& findings);

// Expose the threat pattern table size for tests/sanity checks.
std::size_t threat_pattern_count();

// Structural limits — exposed for tests.
inline constexpr int kMaxFileCount = 50;
inline constexpr int kMaxTotalSizeKB = 1024;
inline constexpr int kMaxSingleFileKB = 256;

}  // namespace hermes::skills::guard
