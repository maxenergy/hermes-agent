// Skill discovery and metadata utilities — scan builtin, optional, and
// user-installed skill directories, parse SKILL.md frontmatter, and apply
// platform / disabled-list gating.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::skills {

struct SkillMetadata {
    std::string name;
    std::string description;
    std::string version;
    std::vector<std::string> platforms;   // "cli", "telegram", etc.
    std::vector<std::string> categories;
    std::filesystem::path path;
    bool enabled = true;
};

// Scan all skill directories (builtin + optional + user-installed).
std::vector<std::filesystem::path> get_all_skills_dirs();

// Iterate skill index files (index.json or SKILL.md with frontmatter).
std::vector<SkillMetadata> iter_skill_index();

// Parse YAML frontmatter from markdown: "---\nkey: val\n---\ncontent".
// Returns {parsed-json, body-after-frontmatter}.  If no frontmatter is
// found the first element is a null JSON and the second is the full input.
std::pair<nlohmann::json, std::string> parse_frontmatter(std::string_view markdown);

// Extract description/conditions from frontmatter.
std::string extract_skill_description(const nlohmann::json& frontmatter);
std::vector<std::string> extract_skill_conditions(const nlohmann::json& frontmatter);

// Platform gating — returns true when the skill's platforms list is empty
// (meaning "all platforms") or contains the given platform string.
bool skill_matches_platform(const SkillMetadata& skill, std::string_view platform);

// Disabled skills from config — reads "disabled_skills" array.
std::vector<std::string> get_disabled_skill_names(const nlohmann::json& config);

}  // namespace hermes::skills
