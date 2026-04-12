// OpenClaw -> Hermes migration tool.
//
// `hermes claw migrate` reads assets from `~/.openclaw/` and imports
// them into `$HERMES_HOME` (SOUL.md, MEMORY.md/USER.md, skills, command
// allowlist, messaging settings, API keys, TTS assets).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::cli::claw {

struct MigrateOptions {
    bool dry_run = false;
    bool overwrite = false;
    std::string preset = "full";  // full|user-data|no-secrets
    std::filesystem::path workspace_target;
    std::filesystem::path openclaw_dir;  // default: ~/.openclaw
    // Optional override for destination (tests); when empty, uses
    // hermes::core::path::get_hermes_home().
    std::filesystem::path hermes_home_override;
};

struct MigrationResult {
    std::vector<std::string> imported;
    std::vector<std::string> skipped;
    std::vector<std::string> errors;
    int item_count = 0;
};

// Run the migration honoring preset + flags.
MigrationResult migrate(const MigrateOptions& opts);

// Individual migrators — exposed for targeted testing.
void migrate_soul(const MigrateOptions& opts, MigrationResult& result);
void migrate_memories(const MigrateOptions& opts, MigrationResult& result);
void migrate_skills(const MigrateOptions& opts, MigrationResult& result);
void migrate_command_allowlist(const MigrateOptions& opts,
                               MigrationResult& result);
void migrate_messaging(const MigrateOptions& opts, MigrationResult& result);
void migrate_api_keys(const MigrateOptions& opts, MigrationResult& result);
void migrate_tts_assets(const MigrateOptions& opts, MigrationResult& result);
void migrate_agents_md(const MigrateOptions& opts, MigrationResult& result);

// Internal helper (exposed for tests): resolves the effective destination
// hermes home, honoring opts.hermes_home_override.
std::filesystem::path resolve_hermes_home(const MigrateOptions& opts);

// Internal helper: resolves effective openclaw dir (default ~/.openclaw).
std::filesystem::path resolve_openclaw_dir(const MigrateOptions& opts);

// The allowlist of environment variables copied by migrate_api_keys.
const std::vector<std::string>& api_key_allowlist();

}  // namespace hermes::cli::claw
