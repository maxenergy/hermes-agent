// agent/skill_utils — lightweight skill metadata utilities.
//
// C++17 port of ``agent/skill_utils.py`` (443 LoC).  Used by the prompt
// builder and the skills tool to parse SKILL.md frontmatter, determine
// platform compatibility, locate enabled skill directories, and extract
// declared config variables.
//
// This module intentionally stays free of tool-registry and CLI-config
// imports so it can be included from almost any layer.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::agent::skill_utils {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

// Storage prefix: all skill config vars are stored under ``skills.config.*``
// in config.yaml.  Skill authors declare logical keys (e.g. ``"wiki.path"``);
// the system adds this prefix for storage and strips it for display.
inline constexpr const char* kSkillConfigPrefix = "skills.config";

// Directory names excluded when walking a skills tree.
inline const std::unordered_set<std::string>& excluded_skill_dirs();

// ---------------------------------------------------------------------------
// Data types.
// ---------------------------------------------------------------------------

// Parsed frontmatter block + the remaining markdown body.
struct Frontmatter {
    nlohmann::json data;  // always an object (possibly empty)
    std::string body;
};

// Conditional activation fields extracted from frontmatter metadata.
struct SkillConditions {
    std::vector<std::string> fallback_for_toolsets;
    std::vector<std::string> requires_toolsets;
    std::vector<std::string> fallback_for_tools;
    std::vector<std::string> requires_tools;
};

// Declared config variable.  ``default_value`` is optional; ``prompt``
// defaults to ``description`` when the skill didn't provide one.
struct SkillConfigVar {
    std::string key;
    std::string description;
    std::string prompt;
    std::optional<nlohmann::json> default_value;
    std::string skill;  // set by discover_all_skill_config_vars()
};

// ---------------------------------------------------------------------------
// YAML helpers.
// ---------------------------------------------------------------------------

// Parse ``content`` as YAML and return it as nlohmann::json.  Returns
// ``nullopt`` on parse failure.  Scalars are coerced to their most natural
// JSON type (bool / int / double / string).
std::optional<nlohmann::json> parse_yaml(const std::string& content);

// Parse YAML frontmatter from a markdown string.  If the content doesn't
// start with ``---`` the frontmatter is returned empty and the body is the
// full input.
Frontmatter parse_frontmatter(const std::string& content);

// ---------------------------------------------------------------------------
// Platform + enablement.
// ---------------------------------------------------------------------------

// Map ``macos``/``linux``/``windows`` to the corresponding sys.platform
// prefix (``darwin``/``linux``/``win32``).  Unknown values are returned
// lowercased unchanged.
std::string normalize_platform_name(const std::string& raw);

// Return the current platform ID (``darwin`` / ``linux`` / ``win32`` / ...).
// Determined at compile time via the usual __APPLE__ / _WIN32 / __linux__
// predicates.
std::string current_platform_id();

// True when the skill's ``platforms`` field either is missing or includes
// the current platform.
bool skill_matches_platform(const nlohmann::json& frontmatter);

// Parse the ``skills.disabled`` / ``skills.platform_disabled.<p>`` lists
// from ``config.yaml`` and return the resolved set for the given platform
// (or the active session platform when ``platform`` is empty).
std::unordered_set<std::string> get_disabled_skill_names(
    const std::string& platform = {});

// ---------------------------------------------------------------------------
// External skills directories.
// ---------------------------------------------------------------------------

// Read ``skills.external_dirs`` from config.yaml, expand ``~`` and
// ``${VAR}`` references, and return only existing directories.  Entries
// that resolve to the local ``<HERMES_HOME>/skills`` are silently skipped.
std::vector<fs::path> get_external_skills_dirs();

// Return all skill directories: local ``<HERMES_HOME>/skills`` first,
// followed by any configured external dirs.  The local entry is included
// even when the directory does not yet exist (callers handle that).
std::vector<fs::path> get_all_skills_dirs();

// ---------------------------------------------------------------------------
// Condition / config / description extraction.
// ---------------------------------------------------------------------------

SkillConditions extract_skill_conditions(const nlohmann::json& frontmatter);

std::vector<SkillConfigVar> extract_skill_config_vars(
    const nlohmann::json& frontmatter);

// Walk every enabled skill directory, parse SKILL.md frontmatter, and
// return a deduplicated list of declared config vars (the first skill to
// claim a given key wins).  ``skill`` is set on each entry for UI
// attribution.
std::vector<SkillConfigVar> discover_all_skill_config_vars();

// Resolve current values from config.yaml's ``skills.config.*`` namespace.
// Path-like string values are expanded with ``~`` and ``${VAR}``.  The
// returned JSON object maps logical (un-prefixed) keys to their values.
nlohmann::json resolve_skill_config_values(
    const std::vector<SkillConfigVar>& config_vars);

// Truncate to 60 chars with ellipsis; returns empty when missing.
std::string extract_skill_description(const nlohmann::json& frontmatter);

// ---------------------------------------------------------------------------
// Directory walking.
// ---------------------------------------------------------------------------

// Walk ``skills_dir`` yielding sorted paths matching ``filename`` (typically
// ``SKILL.md`` or ``AGENT.md``).  Excluded directories (``.git``/``.github``/
// ``.hub``) are skipped.  Returned paths are sorted by their path relative
// to ``skills_dir`` for deterministic output.
std::vector<fs::path> iter_skill_index_files(const fs::path& skills_dir,
                                             const std::string& filename);

// Expand ``~`` and ``${VAR}`` in a path string.
std::string expand_path(const std::string& raw);

// Walk a nested JSON object following a dotted key; returns nullptr when
// any intermediate step is missing or not an object.
const nlohmann::json* resolve_dotpath(const nlohmann::json& config,
                                      const std::string& dotted_key);

// Normalize a YAML value (scalar or list) into a set of trimmed strings,
// dropping empty entries.
std::unordered_set<std::string> normalize_string_set(
    const nlohmann::json& values);

}  // namespace hermes::agent::skill_utils
