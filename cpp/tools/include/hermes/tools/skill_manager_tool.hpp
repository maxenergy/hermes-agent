// Skill manager tool — list/search/install/uninstall/update + create/delete.
//
// Mirrors ``tools/skill_manager_tool.py`` for local operations and wires
// the remote Hub actions (list_available / search / install / update)
// through ``hermes::skills::SkillsHub`` when HERMES_SKILLS_HUB_URL is
// configured, falling back to the installed skill directory otherwise.
// Helpers below are pulled out of the dispatch handler so the validation
// and frontmatter-parsing logic can be unit-tested independently.
#pragma once

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

// Filesystem and content limits — mirror the Python constants.
constexpr std::size_t kSkillMaxNameLength = 64;
constexpr std::size_t kSkillMaxDescriptionLength = 1024;
constexpr std::size_t kSkillMaxContentChars = 100'000;
constexpr std::size_t kSkillMaxFileBytes = 1'048'576;

// Subdirectories allowed under a skill root (write_file/remove_file).
const std::vector<std::string>& allowed_skill_subdirs();

// Returns the resolved skills-root path.  Honoured by all helpers.
std::filesystem::path skills_root();

void register_skill_manager_tools(ToolRegistry& registry);

// ---- Validation helpers --------------------------------------------------

// Validate a skill name.  Returns an error string when invalid; empty
// on success.  Names must match ``[a-z0-9][a-z0-9._-]*`` and stay within
// kSkillMaxNameLength.
std::string validate_skill_name(std::string_view name);

// Validate an optional category segment.  Empty input returns empty.
std::string validate_skill_category(std::string_view category);

// Validate a relative file path within a skill directory.  Rejects
// absolute paths, parent traversal, and paths outside the allowed
// subdirectories.
std::string validate_skill_file_path(std::string_view rel);

// ---- Frontmatter parsing -------------------------------------------------

struct SkillFrontmatter {
    std::string name;
    std::string description;
    std::vector<std::string> required_credential_files;
    std::vector<std::string> tags;
    std::string version;
    nlohmann::json raw;  // any extra YAML keys preserved as JSON
};

// Parse YAML-style frontmatter from a SKILL.md body.  Only the bounded
// keys above are recognised; everything else is dropped.  A document
// without an opening ``---`` line returns an empty struct.
SkillFrontmatter parse_skill_frontmatter(std::string_view body);

// Read a SKILL.md file and return its parsed frontmatter.  Returns an
// empty struct on read errors.
SkillFrontmatter read_skill_frontmatter(const std::filesystem::path& skill_md);

// ---- Listing -------------------------------------------------------------

struct InstalledSkill {
    std::string name;
    std::string description;
    std::string version;
    std::filesystem::path path;
    bool has_skill_md = false;
    bool has_index_json = false;
};

// Enumerate every skill installed under ``root``.  Skills are recognised
// by an ``index.json`` or ``SKILL.md`` at the top of each subdirectory.
std::vector<InstalledSkill> enumerate_installed_skills(
    const std::filesystem::path& root);

// Render the list_installed response payload.
nlohmann::json render_installed_list(
    const std::vector<InstalledSkill>& installed);

// ---- Local search --------------------------------------------------------

// Substring search across installed skill names + descriptions.  Case
// insensitive.  Returns the matching subset preserving root order.
std::vector<InstalledSkill> search_installed_skills(
    const std::vector<InstalledSkill>& installed, std::string_view query);

// ---- Path safety ---------------------------------------------------------

// Returns ``true`` if ``candidate`` resolves to a path under ``root``.
// Uses weakly_canonical so non-existent files are still tested cleanly.
bool path_under_root(const std::filesystem::path& candidate,
                     const std::filesystem::path& root);

}  // namespace hermes::tools
