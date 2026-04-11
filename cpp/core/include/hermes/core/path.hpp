// Filesystem helpers mirroring hermes_constants.py — all paths are
// expressed via std::filesystem::path and are import-safe (no hidden IO
// beyond what the Python originals perform).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace hermes::core::path {

namespace fs = std::filesystem;

// Returns $HERMES_HOME if set, otherwise $HOME/.hermes.
fs::path get_hermes_home();

// Returns $HOME/.hermes, always — ignores $HERMES_HOME. This matches
// the Python `get_profiles_root` / `get_default_hermes_root` callers
// that specifically want the native install root.
fs::path get_default_hermes_root();

// Returns $HOME/.hermes/profiles — HOME-anchored, NOT HERMES_HOME-anchored.
fs::path get_profiles_root();

// Returns `get_hermes_home() / "optional-skills"`.
fs::path get_optional_skills_dir();

// Resolve a subdirectory with backward compatibility:
//  - if `<home>/<old_name>` exists, return that;
//  - otherwise return `<home>/<new_subpath>`.
// `old_name` may be empty to mean "no legacy fallback".
fs::path get_hermes_dir(std::string_view new_subpath, std::string_view old_name = "");

// User-facing string: `~/.hermes`, `~/.hermes/profiles/<name>`, or the
// absolute path when HERMES_HOME points outside $HOME.
std::string display_hermes_home();

}  // namespace hermes::core::path
