// Depth-port helpers for ``tools/skills_tool.py``.  The main skills_tool
// layer already handles the registered tool entrypoints; this file
// exposes the pure helpers the Python code uses to normalise skill
// metadata, estimate tokens, parse tags, split categories off skill
// paths, classify readiness, and format setup notes / gateway hints.
//
// All helpers here are pure: no filesystem IO, no env access, no config
// loading.  The intent is that cpp/tests pin the exact behaviour the
// Python implementation exposes so both engines agree on edge cases.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools::skills_depth {

// ---- Readiness status ---------------------------------------------------

enum class ReadinessStatus {
    Ready,
    MissingEnv,
    MissingCommand,
    Incompatible,
    Unknown,
};

// Parse the string form used by Python's ``SkillReadinessStatus`` enum.
// Case-insensitive.  Unknown strings map to ``ReadinessStatus::Unknown``.
ReadinessStatus parse_readiness(std::string_view raw);

// Return the canonical Python-style name for a status value.
std::string readiness_name(ReadinessStatus status);

// ---- Prerequisite / setup normalisation --------------------------------

// Normalise the Python ``_normalize_prerequisite_values`` contract:
// - null / empty -> []
// - single string -> [string] (after strip; empty string -> [])
// - list -> preserved (items coerced to string; empty entries dropped)
std::vector<std::string> normalise_prerequisite_values(const nlohmann::json& value);

struct PrereqLists {
    std::vector<std::string> env_vars;
    std::vector<std::string> commands;
};

// Extract the ``prerequisites.env_vars`` and ``prerequisites.commands``
// lists from a frontmatter dict.  Missing / non-dict ``prerequisites``
// yields empty lists.  Matches ``_collect_prerequisite_values``.
PrereqLists collect_prerequisite_values(const nlohmann::json& frontmatter);

struct CollectSecret {
    std::string env_var;
    std::string prompt;
    bool secret = true;
    std::string provider_url;  // may be empty
};

struct SetupMetadata {
    std::optional<std::string> help;  // empty-string stripped → nullopt
    std::vector<CollectSecret> collect_secrets;
};

// Port of ``_normalize_setup_metadata``.  Non-dict ``setup`` yields an
// empty SetupMetadata.  Single-dict ``collect_secrets`` is promoted to a
// list.  Entries without ``env_var`` are dropped.  Missing ``prompt``
// defaults to ``"Enter value for <env_var>"``.
SetupMetadata normalise_setup_metadata(const nlohmann::json& frontmatter);

struct RequiredEnvEntry {
    std::string name;
    std::string prompt;
    std::string help;          // empty when unset
    std::string required_for;  // empty when unset
};

// Full port of ``_get_required_environment_variables``.  Returns the
// merged list of required env vars from the new structured schema plus
// the legacy ``setup.collect_secrets`` and ``prerequisites.env_vars``
// fields, deduped by name in insertion order.  Names that do not match
// ``[A-Z_][A-Z0-9_]*`` are rejected.  When ``legacy_env_vars`` is null
// it is recomputed from ``frontmatter.prerequisites.env_vars``.
std::vector<RequiredEnvEntry> get_required_environment_variables(
    const nlohmann::json& frontmatter,
    const std::optional<std::vector<std::string>>& legacy_env_vars = std::nullopt);

// Validate an env var name against ``_ENV_VAR_NAME_RE``.
bool is_valid_env_var_name(std::string_view name);

// ---- Frontmatter parsing (lightweight) ---------------------------------

// Rough token estimate — Python uses ``len(content) // 4``.
std::size_t estimate_tokens(std::string_view content);

// Parse a tags value which may be a YAML-parsed list, a bracketed
// string (``"[a, b, c]"``), or a comma-separated string (``"a, b, c"``).
// Returns the trimmed list with empty entries dropped.  Matches
// ``_parse_tags``.
std::vector<std::string> parse_tags(const nlohmann::json& raw);

// Extract a single-segment category from a SKILL.md path relative to
// one of the known skills roots.  Returns empty string when the path
// is not under any root or when there aren't enough segments.  Mirrors
// ``_get_category_from_path``.
std::string category_from_relative_path(std::string_view rel_path);

// ---- Disabled-skill detection ------------------------------------------

// Normalise a platform identifier: lower-case + strip whitespace.
std::string normalise_platform_id(std::string_view raw);

// Given a list of disabled names (from config.skills.disabled) plus an
// optional per-platform disabled list, return ``true`` when ``name``
// is disabled on the active platform.  Comparison is case-insensitive.
// Mirrors the intersection check in ``_is_skill_disabled``.
bool is_skill_disabled(std::string_view name,
                       const std::vector<std::string>& disabled_all,
                       const std::vector<std::string>& disabled_on_platform);

// ---- Gateway surface detection -----------------------------------------

// Return the default gateway surfaces (Telegram/Discord/Slack/…) that
// trigger the "use /setup from an admin chat" hint when env vars are
// missing.  Matches ``_is_gateway_surface``'s allowlist.
const std::vector<std::string>& gateway_surface_names();

// Return ``true`` when ``platform`` (lower-cased) is one of the
// gateway surface names.
bool is_gateway_surface(std::string_view platform);

// Format the "add these env vars in ~/.hermes/.env" hint used by
// ``_gateway_setup_hint`` and ``_build_setup_note``.  ``missing`` is
// the list of still-needed variable names.  Returns empty when the
// list is empty.
std::string format_setup_hint(const std::vector<std::string>& missing,
                              bool on_gateway);

// Compute the subset of required env names that are still missing
// given a callable that reports whether each name is persisted.
// Mirrors ``_remaining_required_environment_names``.
using EnvPersistedFn = bool (*)(std::string_view);
std::vector<std::string> remaining_required_env_names(
    const std::vector<RequiredEnvEntry>& required,
    EnvPersistedFn is_persisted);

// ---- Skill listing / filtering -----------------------------------------

struct SkillBriefEntry {
    std::string name;
    std::string description;
    std::string category;
    std::vector<std::string> tags;
    std::size_t tokens = 0;
};

// Build the compact list payload emitted by ``skills_list`` — a JSON
// object with ``count`` and ``skills`` keys.
nlohmann::json render_skills_list(const std::vector<SkillBriefEntry>& entries);

// Filter a list of entries by category.  Empty category matches
// everything.  Matches Python's filter step in ``skills_list``.
std::vector<SkillBriefEntry> filter_by_category(
    const std::vector<SkillBriefEntry>& entries, std::string_view category);

// Bucket entries by category (preserving first-seen order of the
// categories).  Entries with empty categories end up under ``"misc"``.
// Matches the ``skills_categories`` payload structure.
nlohmann::json group_by_category(const std::vector<SkillBriefEntry>& entries);

// ---- Frontmatter-block extraction --------------------------------------

struct FrontmatterSplit {
    std::string yaml_block;  // between the two ``---`` fences
    std::string body;        // everything after the closing fence
    bool ok = false;         // true only when both fences were found
};

// Split a SKILL.md body into its YAML frontmatter and markdown body.
// Returns ``ok=false`` when there is no opening ``---`` or the block
// is not closed.  Does not parse the YAML — callers do that separately.
FrontmatterSplit split_frontmatter(std::string_view content);

}  // namespace hermes::tools::skills_depth
