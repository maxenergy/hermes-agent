// Depth-port helpers for ``tools/skill_manager_tool.py``.  The main
// skill_manager_tool.hpp already exposes the basic validators; this file
// adds the deeper helpers used by create/edit/patch/write_file:
// content-size validation, relative-path resolution, action routing,
// error-payload shaping, and atomic-write-name derivation.  None of
// these helpers touch the filesystem — they are pure logic mirroring the
// Python behaviour.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools::skill_manager::depth {

// ---- Action routing -----------------------------------------------------

enum class Action {
    List,
    Search,
    Install,
    Uninstall,
    Update,
    Create,
    Edit,
    Patch,
    Delete,
    WriteFile,
    RemoveFile,
    View,
    Unknown,
};

// Parse the ``action`` argument.  Case-insensitive.  Empty / unknown
// strings return ``Unknown`` so callers can surface a clear error.
Action parse_action(std::string_view raw);

// Canonical string for an action value.
std::string action_name(Action action);

// Return the actions that require a ``name`` argument.  Used to pre-
// validate inputs before attempting any filesystem work.
bool action_requires_name(Action action);

// Return the actions that require a ``content`` argument.
bool action_requires_content(Action action);

// ---- Content-size validation -------------------------------------------

// Maximum allowed SKILL.md content size (characters).  Mirrors
// ``MAX_SKILL_CONTENT_CHARS`` in Python.
constexpr std::size_t kMaxSkillContentChars = 100'000u;

// Maximum allowed supporting-file size (bytes).  Mirrors
// ``MAX_SKILL_FILE_BYTES`` in Python.
constexpr std::size_t kMaxSkillFileBytes = 1'048'576u;

// Return an error string when ``content`` is too large, else empty.
// ``label`` is the filename used in the error message.
std::string validate_content_size(std::size_t chars, std::string_view label);

// Return an error string when ``bytes`` is too large for a supporting
// file, else empty.
std::string validate_file_bytes(std::size_t bytes, std::string_view label);

// ---- Frontmatter structural check --------------------------------------

struct FrontmatterCheck {
    bool ok = false;
    std::string error;
    std::string yaml_block;  // when ok, the raw YAML string
    std::string body;        // when ok, the markdown body
};

// Verify that ``content`` has an opening ``---`` fence and a closing
// ``\n---`` fence.  Does not parse the YAML — callers feed the yaml
// block to a YAML parser to check for name/description keys.  This
// mirrors the structural half of ``_validate_frontmatter``.
FrontmatterCheck check_frontmatter_structure(std::string_view content);

// Validate that a parsed frontmatter dict has the required keys and
// that description is within the size limit.  Returns an error string
// on failure, empty on success.  Mirrors the second half of
// ``_validate_frontmatter``.
std::string validate_frontmatter_keys(const nlohmann::json& parsed);

// ---- Relative-path resolution ------------------------------------------

// Normalise a relative file path: collapse ``.`` segments, reject ``..``
// and absolute paths.  Returns the cleaned path (using ``/`` separators)
// and an error string.  The error is empty on success.
struct NormalisedRelPath {
    std::string cleaned;
    std::string error;
};

NormalisedRelPath normalise_relative_path(std::string_view rel);

// Return ``true`` when the first path segment of ``rel`` is one of the
// allowed skill subdirs (references/templates/scripts/assets).  Assumes
// the path has already been normalised.
bool first_segment_is_allowed(std::string_view rel);

// Generate the ``tmp-…`` filename used for atomic writes — matches
// ``tempfile.mkstemp(dir=..., prefix=f".{name}.tmp.")``.  Pure: the
// "random" suffix is deterministic given ``seed`` so tests can pin.
std::string atomic_temp_name(std::string_view target_name, unsigned seed);

// ---- Response payloads --------------------------------------------------

// Build a ``{"success": false, "error": msg}`` payload.
nlohmann::json error_payload(std::string_view message);

// Build a ``{"success": true, "message": msg}`` payload.
nlohmann::json success_message_payload(std::string_view message);

// Build a "skill not found" error response.
nlohmann::json not_found_payload(std::string_view skill_name);

// Build a "patch would break structure" error — prefixes the underlying
// structural error with the standard wrapper phrase.
nlohmann::json structural_break_payload(std::string_view underlying);

// ---- Search / filter ---------------------------------------------------

struct MinimalSkill {
    std::string name;
    std::string description;
};

// Case-insensitive substring search across name + description.  Returns
// the matching subset preserving input order.
std::vector<MinimalSkill> substring_search(
    const std::vector<MinimalSkill>& skills, std::string_view query);

// ---- Patch-mode replacement --------------------------------------------

struct PatchResult {
    std::string output;
    std::size_t replacements = 0;
    std::string error;  // empty on success
};

// Exact-substring find-and-replace.  When ``replace_all`` is false, the
// match must be unique — more than one occurrence yields an error.
// A zero-match yields an error.  Empty ``old_string`` yields an error.
// Mirrors the basic contract of ``fuzzy_find_and_replace`` (the fuzzy
// path is Python-only; the C++ engine uses exact substrings).
PatchResult exact_replace(std::string_view content, std::string_view old_s,
                          std::string_view new_s, bool replace_all);

}  // namespace hermes::tools::skill_manager::depth
