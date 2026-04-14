// Skills tools — list, view, and explore installed skills.
//
// Port of tools/skills_tool.py.  Skills live in ~/.hermes/skills/ as
// directories containing a SKILL.md file (with optional YAML
// frontmatter) and supporting references/templates/assets sub-trees.
//
// This module provides three tools:
//   - skills_list        → enumerate skills (optionally by category)
//   - skill_view         → load SKILL.md or a sub-file
//   - skills_categories  → enumerate category folders (tier-0 disclosure)
//
// Plus a small set of utility helpers (exposed for tests):
//   - parse_frontmatter
//   - skill_matches_platform
//   - get_category_from_path
//   - estimate_tokens
//   - parse_tags
#pragma once

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::tools::skills {

// Anthropic-recommended progressive-disclosure limits.
constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxDescriptionLength = 1024;

/// Return the path to ~/.hermes/skills/ (honors HERMES_HOME).
std::filesystem::path skills_dir();

/// Parse a YAML-ish frontmatter block from the start of |content|.
///
/// Supports the simple subset used in SKILL.md files:
///   - key: value
///   - key: [a, b, c]            (inline list)
///   - key:                      (block list / mapping continuation)
///       - a
///       - b
///
/// Returns a pair {frontmatter, body}.  When no frontmatter is present,
/// returns {empty_object, content}.
std::pair<nlohmann::json, std::string> parse_frontmatter(
    std::string_view content);

/// Return true when the frontmatter's "platforms" field permits the
/// current OS.  Missing field → permitted on all platforms.
bool skill_matches_platform(const nlohmann::json& frontmatter);

/// If |skill_md| lies under ~/.hermes/skills/<category>/<skill>/SKILL.md,
/// return <category>.  Otherwise returns empty string.
std::string get_category_from_path(const std::filesystem::path& skill_md);

/// Rough token estimate: chars / 4.
std::size_t estimate_tokens(std::string_view content);

/// Split a tags value (list, bracket-wrapped string, or comma-separated
/// string) into a vector of trimmed tag strings.
std::vector<std::string> parse_tags(const nlohmann::json& tags_value);

/// Register all skills tools (skills_list, skill_view, skills_categories).
void register_skills_tools(hermes::tools::ToolRegistry& registry);

}  // namespace hermes::tools::skills

namespace hermes::tools {

// Back-compat shim — existing callers use this free function.
inline void register_skills_tools(ToolRegistry& registry) {
    skills::register_skills_tools(registry);
}

}  // namespace hermes::tools
