// skills_tool — depth port of Python tools/skills_tool.py.
//
// Public helpers to:
//   * normalize skill frontmatter (prerequisites, setup, required env vars),
//   * classify readiness (available / setup_needed / unsupported),
//   * parse tags, estimate tokens, truncate description,
//   * check platform match for frontmatter,
//   * scan skills directories and produce list/view output,
//   * build gateway setup hints and setup notes.
//
// These functions are deterministic (no network, minimal IO) and designed
// to be exercised from unit tests.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::skills {

// ---------------------------------------------------------------------------
// Limits & constants (mirror the Python module).
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxSkillNameLength = 64;
inline constexpr std::size_t kMaxSkillDescriptionLength = 1024;

enum class SkillReadinessStatus { Available, SetupNeeded, Unsupported };

std::string to_string(SkillReadinessStatus s);

// ---------------------------------------------------------------------------
// Small parsing helpers.
// ---------------------------------------------------------------------------

// Rough 4-chars-per-token estimate used for list output token budgets.
std::size_t estimate_tokens(std::string_view content);

// Parse a "tags" frontmatter value. Accepts:
//   * a JSON array of strings (preferred),
//   * a JSON string formatted as "[a, b]" or "a, b".
std::vector<std::string> parse_tags(const nlohmann::json& tags_value);

// Validate a candidate env-var name (ASCII, starts with letter/_).
bool is_valid_env_var_name(std::string_view name);

// Valid platform strings accepted in "platforms" frontmatter:
//   "macos" / "linux" / "windows" / "darwin" / "win32".  Maps user-friendly
//   names onto the sys.platform prefix used internally.
std::string normalize_platform_token(std::string_view token);

// Given the current platform prefix (usually sys.platform prefix), decide
// whether a skill declares compatibility.  An empty or missing "platforms"
// list matches every platform.
bool skill_matches_platform(const nlohmann::json& frontmatter,
                            std::string_view current_platform);

// Truncate a description to kMaxSkillDescriptionLength with a "..." suffix
// when overflow occurred.
std::string truncate_description(std::string_view description);

// ---------------------------------------------------------------------------
// Prerequisites + setup metadata (from frontmatter).
// ---------------------------------------------------------------------------

struct Prerequisites {
    std::vector<std::string> env_vars;   // legacy env_vars list.
    std::vector<std::string> commands;   // legacy command-check list.
};

Prerequisites collect_prerequisites(const nlohmann::json& frontmatter);

struct CollectSecret {
    std::string env_var;
    std::string prompt;
    std::string provider_url;  // empty when absent
    bool secret = true;
};

struct SetupMetadata {
    std::optional<std::string> help;
    std::vector<CollectSecret> collect_secrets;
};

SetupMetadata normalize_setup_metadata(const nlohmann::json& frontmatter);

struct RequiredEnvVar {
    std::string name;
    std::string prompt;
    std::string help;         // optional — empty means absent
    std::string required_for; // optional — empty means absent
};

// Produce the full list of required env vars by merging:
//   * frontmatter.required_environment_variables entries,
//   * setup.collect_secrets entries,
//   * legacy prerequisites.env_vars entries (deduped).
// Names that fail `is_valid_env_var_name` are silently dropped.
std::vector<RequiredEnvVar> get_required_environment_variables(
    const nlohmann::json& frontmatter);

// Gateway setup hint message surfaced when a required secret is missing and
// the caller runs on a messaging gateway that cannot collect it interactively.
std::string gateway_setup_hint();

// Build a user-facing "setup needed" note or return nullopt when the skill
// is already ready.
std::optional<std::string> build_setup_note(
    SkillReadinessStatus status,
    const std::vector<std::string>& missing,
    std::string_view setup_help = {});

// ---------------------------------------------------------------------------
// Dot-env parsing — shared across tools/skills, the CLI, and the gateway.
// ---------------------------------------------------------------------------

// Parse a dot-env style file (KEY=VALUE per line, '#' comments, quoted values).
std::unordered_map<std::string, std::string> parse_dotenv(std::string_view body);

// Load ~/.hermes/.env (respects HERMES_HOME).
std::unordered_map<std::string, std::string> load_env_file();

// True when the given name has a non-empty value either in the snapshot or
// in the live process environment.
bool is_env_var_persisted(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& snapshot);

// Given a required list + a snapshot of env vars + a set of names the
// secret-capture step couldn't satisfy, return the still-missing names.
std::vector<std::string> remaining_required_environment_names(
    const std::vector<RequiredEnvVar>& required,
    const std::vector<std::string>& capture_missing,
    const std::unordered_map<std::string, std::string>& snapshot);

// Classify readiness for a skill given its frontmatter + a snapshot of the
// environment.  When any required env var is missing -> SetupNeeded.  Empty
// missing list -> Available.
struct ReadinessReport {
    SkillReadinessStatus status = SkillReadinessStatus::Available;
    std::vector<std::string> missing_env_vars;
    std::optional<std::string> setup_help;
    std::optional<std::string> note;
};

ReadinessReport classify_readiness(
    const nlohmann::json& frontmatter,
    const std::unordered_map<std::string, std::string>& env_snapshot);

// ---------------------------------------------------------------------------
// Skill discovery.
// ---------------------------------------------------------------------------

struct SkillListEntry {
    std::string name;
    std::string description;
    std::string category;   // empty when no category detected
    std::filesystem::path path;
};

// Compute the category name from a skill path by examining its parents
// relative to any known skills dir.  Returns empty string when no
// category component can be identified.
std::string category_from_path(const std::filesystem::path& skill_md,
                               const std::vector<std::filesystem::path>& skills_dirs);

// Load a DESCRIPTION.md file's description (frontmatter or first non-header
// line).  Returns nullopt when the file is missing or unreadable.
std::optional<std::string> load_category_description(
    const std::filesystem::path& category_dir);

// Scan a single skills directory and return every skill entry under it.
// Recurses and stops at any path component listed in `excluded`.
std::vector<SkillListEntry> find_skills_in_dir(
    const std::filesystem::path& scan_dir,
    std::string_view current_platform);

// Merge & dedupe entries from several scan directories.  Skills appearing
// first win (local overrides external).
std::vector<SkillListEntry> merge_skill_lists(
    std::vector<std::vector<SkillListEntry>> dir_results,
    const std::vector<std::string>& disabled_names);

// Given the full on-disk content of a SKILL.md (already loaded), produce
// a JSON envelope like the Python skill_view() tool returns (name,
// description, body, tokens).
nlohmann::json build_skill_view_envelope(std::string_view skill_name,
                                          std::string_view skill_md_content);

}  // namespace hermes::skills
