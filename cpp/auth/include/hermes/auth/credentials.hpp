// File-based credential store backed by `<HERMES_HOME>/.env`.
//
// Scope for Phase 1 is intentionally minimal: no OAuth, no Copilot, no
// Nous subscription.  Those land in Phase 13.  This module is just
// enough to read/write simple `KEY=value` pairs with 0600 permissions
// so the CLI setup wizard + tools can persist API keys.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::auth {

// Store (or update) a credential in `<HERMES_HOME>/.env`.  Performs a
// whole-file rewrite via `hermes::core::atomic_write`, then chmods the
// result to 0600 on POSIX.  Throws `std::runtime_error` on IO failure.
void store_credential(std::string_view key, std::string_view value);

// Look up a credential.  Returns the value from `<HERMES_HOME>/.env`
// if present; otherwise falls back to `std::getenv` so callers can
// transparently read from the real process environment.
std::optional<std::string> get_credential(std::string_view key);

// Remove a single key, preserving the rest of the file.  No-op when
// the key is not present.
void clear_credential(std::string_view key);

// Delete `<HERMES_HOME>/.env` entirely.
void clear_all_credentials();

// Return every key declared in `<HERMES_HOME>/.env` (without values).
// Order matches the file order.  Returns an empty vector if the file
// does not exist.
std::vector<std::string> list_credential_keys();

}  // namespace hermes::auth
