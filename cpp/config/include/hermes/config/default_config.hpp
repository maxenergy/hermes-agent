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
//
// v15 (upstream commits 762f7e97 / 285bb2b9 / 64b35471 / 1ca9b197):
//   * approvals.cron_mode = "deny"
//   * code_execution.mode = "project"
//   * browser.cdp_url = ""
//   * network.force_ipv4 = false
constexpr int kCurrentConfigVersion = 15;

}  // namespace hermes::config
