// Atomic, comment-preserving rewriter for dotenv-style files.
//
// Complements `hermes::core::env::load_dotenv` (read-side) with a
// minimal write-side tool used by the config migration path:
//
//   * v8 → v9 wipes the retired `ANTHROPIC_TOKEN` slot.
//   * v12 → v13 wipes the now-dead `LLM_MODEL` / `OPENAI_MODEL` slots.
//
// Both were previously left to the Python setup wizard because the
// C++ port had no `.env` writer.  This fills that gap.
//
// Design notes:
//   * Parse existing lines as-is, match key names out of either
//     `KEY=VALUE` or `export KEY=VALUE` (quoted / unquoted values).
//   * Comment lines (`#…`) and blank lines are preserved verbatim in
//     their original position.
//   * `updates` replaces an existing key's line in-place (preserving
//     any `export ` prefix) or appends a new `KEY=VALUE` line.
//   * `removals` drops the matching line entirely.
//   * Write goes through `hermes::core::atomic_io::atomic_write`
//     (temp file + fsync + rename), so interrupted runs never leave a
//     half-written `.env`.
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hermes::auth {

// Rewrite the dotenv file at `path`:
//   - `updates`: for each (key, new_value), replace the line in place
//     or append a fresh `KEY=VALUE` line at the end if the key is absent.
//   - `removals`: for each key, drop its line outright.
// Comment / blank lines are preserved.
//
// If both `updates` and `removals` are empty the file is not touched.
//
// If the file does not exist:
//   - with non-empty `updates` → it is created and populated.
//   - with only `removals`     → no-op, returns true.
//
// Returns false on read / write / IO errors.  Never throws.
bool rewrite_env_file(const std::filesystem::path& path,
                      const std::unordered_map<std::string, std::string>& updates,
                      const std::unordered_set<std::string>& removals);

// Convenience: drop one key from `<HERMES_HOME>/.env`.
bool clear_env_value(const std::string& key);

// Convenience: set one key in `<HERMES_HOME>/.env` (creating the file
// if it does not yet exist).
bool set_env_value(const std::string& key, const std::string& value);

}  // namespace hermes::auth
