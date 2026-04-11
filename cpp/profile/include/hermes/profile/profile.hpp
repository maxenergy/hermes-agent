// Profile manager — apply the `-p <name>` / `--profile <name>` override
// at process startup, and provide CRUD helpers for the set of profiles
// rooted at `$HOME/.hermes/profiles/`.
//
// CRITICAL INVARIANT (matches Python reference):
//   `get_profiles_root()` is ALWAYS HOME-anchored, not HERMES_HOME-anchored.
//   This lets `hermes -p coder profile list` still enumerate every profile
//   even after the user switches to `~/.hermes/profiles/coder` as the
//   active HERMES_HOME.  Do not "simplify" this to use get_hermes_home().
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::profile {

// Set `HERMES_HOME` to the target profile directory.  Must be called
// BEFORE any code that reads `hermes::core::path::get_hermes_home()`
// (typically the very first thing in `main()`).
//
// - `std::nullopt` or empty string ⇒ default profile (leaves
//   HERMES_HOME untouched; profile_name="" is a no-op).
// - Any other name ⇒ sets HERMES_HOME to
//   `<default_hermes_root>/profiles/<name>` and creates the directory
//   if it does not yet exist.
void apply_profile_override(std::optional<std::string> profile_name);

// Returns the HOME-anchored profiles root (see invariant above).
std::filesystem::path get_profiles_root();

// List every immediate subdirectory of `get_profiles_root()`.  Returns
// an empty vector if the root does not yet exist.  Sort order is
// filesystem-default (no guaranteed order).
std::vector<std::string> list_profiles();

// Create a new profile directory.  Seeds it with a copy of the default
// config if `<default_hermes_root>/config.yaml` exists; otherwise
// writes an empty `config.yaml`.  No-op if the profile already exists.
void create_profile(std::string_view name);

// Delete `<profiles_root>/<name>` recursively.  Throws
// `std::runtime_error` if `name` refers to the currently active profile
// (detected via HERMES_HOME).
void delete_profile(std::string_view name);

// Rename a profile on disk.  Throws `std::runtime_error` if the source
// is the currently active profile (same safety rule as delete).
void rename_profile(std::string_view old_name, std::string_view new_name);

}  // namespace hermes::profile
