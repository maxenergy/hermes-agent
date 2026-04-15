// C++17 port of pure-logic helpers from `hermes_cli/profiles.py` —
// profile-name validation, reserved-name detection, path construction,
// archive-safety checks, completion-script generation.
//
// No filesystem / subprocess side-effects in this header; tests can
// exercise everything with in-memory inputs.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hermes::cli::profiles_helpers {

// ---------------------------------------------------------------------------
// Reserved names & hermes subcommands.
// ---------------------------------------------------------------------------

// Names that must never be used as profile aliases.
const std::unordered_set<std::string>& reserved_names();

// Subcommands of `hermes` — conflict with wrappers of the same name.
const std::unordered_set<std::string>& hermes_subcommands();

// ---------------------------------------------------------------------------
// Validation.
// ---------------------------------------------------------------------------

// True when `name` matches `[a-z0-9][a-z0-9_-]{0,63}` — the accepted
// profile identifier pattern.
bool is_valid_profile_id(const std::string& name);

// Throws std::invalid_argument on invalid names.  "default" is always
// accepted as a special alias for ~/.hermes.
void validate_profile_name(const std::string& name);

// Return a human-readable collision explanation, or std::nullopt if the
// name is safe.  Pure: takes a list of existing binaries in PATH so the
// caller can inject test fixtures.  `own_wrapper_paths` lists binaries
// that are known to be hermes wrappers (safe to overwrite).
std::optional<std::string> check_alias_collision(
    const std::string& name,
    const std::vector<std::string>& existing_commands,
    const std::vector<std::string>& own_wrapper_paths);

// ---------------------------------------------------------------------------
// Path helpers.
// ---------------------------------------------------------------------------

// Resolve a profile name to its HERMES_HOME directory under
// `default_root`.
//   * "default" -> `default_root`
//   * named     -> `default_root / profiles / <name>`
std::filesystem::path get_profile_dir(
    const std::string& name,
    const std::filesystem::path& default_root);

// Return the directory that stores named profiles.
std::filesystem::path get_profiles_root(
    const std::filesystem::path& default_root);

// Return the path to the sticky active_profile file.
std::filesystem::path get_active_profile_path(
    const std::filesystem::path& default_root);

// ---------------------------------------------------------------------------
// Wrapper-script generation.
// ---------------------------------------------------------------------------

// Render the content of `~/.local/bin/<name>`.
std::string render_wrapper_script(const std::string& profile_name);

// True when the given file text is one of our wrappers (i.e. contains
// `hermes -p`).
bool is_hermes_wrapper(const std::string& script_text);

// ---------------------------------------------------------------------------
// Archive path-safety (for `hermes profile import`).
// ---------------------------------------------------------------------------

// Split a tar archive member name into safe path components.
// Mirrors `_normalize_profile_archive_parts` — rejects absolute paths,
// Windows drive letters, empty strings and ".." segments.
// Throws std::invalid_argument on unsafe input.
std::vector<std::string> normalize_archive_member(
    const std::string& member_name);

// ---------------------------------------------------------------------------
// gateway.pid decoding (in-memory).
// ---------------------------------------------------------------------------

// Extract the pid from the contents of a gateway.pid file.
// Accepts either a bare integer ("12345") or a JSON object with a
// "pid" field.  Returns nullopt when the text is empty / invalid.
std::optional<int> parse_gateway_pid_file(const std::string& text);

// ---------------------------------------------------------------------------
// Completion scripts.
// ---------------------------------------------------------------------------

// Canonical contents of `hermes completion bash`.
std::string generate_bash_completion();

// Canonical contents of `hermes completion zsh`.
std::string generate_zsh_completion();

// ---------------------------------------------------------------------------
// Default-profile export — path exclusions.
// ---------------------------------------------------------------------------

// Returns true when `filename` (a top-level name inside ~/.hermes) must
// be excluded when exporting the default profile.
bool is_default_export_excluded(const std::string& filename);

// ---------------------------------------------------------------------------
// Rename validation.
// ---------------------------------------------------------------------------

// Throws std::invalid_argument if the rename is not permitted.
void validate_rename(const std::string& old_name,
                     const std::string& new_name);

}  // namespace hermes::cli::profiles_helpers
