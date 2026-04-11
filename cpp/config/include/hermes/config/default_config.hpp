// Default configuration tree for the Hermes CLI / agent runtime.
//
// Mirrors (at a high level) the `DEFAULT_CONFIG` dict in
// `hermes_cli/config.py`.  We intentionally port only the top-level
// structure required by Phase 1; nested keys will be filled in by later
// phases as the C++ agent runtime catches up with the Python reference.
#pragma once

#include <nlohmann/json.hpp>

namespace hermes::config {

// Returns a reference to a lazily-constructed default config tree.
// Thread-safe via static initialisation.  Callers should copy the
// result before mutating it.
const nlohmann::json& default_config();

// Current config schema version.  Bumping this forces a migration
// pass next time a user's config is loaded.
constexpr int kCurrentConfigVersion = 5;

}  // namespace hermes::config
