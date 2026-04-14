// C++17 port of hermes_cli/claw.py — the full `hermes claw` subcommand.
//
// Complements the existing `claw_migrate.hpp` (which performs the
// actual data transfer) by adding the two surrounding actions:
//
//   - `hermes claw status`    — scan ~/.openclaw and report what is
//                               present (SOUL, memories, skills, env).
//   - `hermes claw cleanup`   — archive leftover OpenClaw directories
//                               to ~/.openclaw-archive-<TIMESTAMP>/ so
//                               the user can safely uninstall the old
//                               toolkit without losing data.
//   - `hermes claw migrate`   — delegates to claw_migrate::migrate().
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::cli::claw_cmd {

// CLI dispatch entry.
int run(int argc, char* argv[]);

// Subcommand handlers.
int cmd_status(const std::vector<std::string>& argv);
int cmd_cleanup(const std::vector<std::string>& argv);
int cmd_migrate(const std::vector<std::string>& argv);

// ----- Pure helpers, exposed for tests. --------------------------

struct OpenClawScan {
    std::filesystem::path root;
    bool exists = false;
    bool has_soul = false;
    bool has_memory = false;
    bool has_user = false;
    bool has_skills = false;
    bool has_env = false;
    int skill_count = 0;
    int session_count = 0;
    std::vector<std::string> findings;   // Human-readable summary lines.
};

// Scan the given OpenClaw root (default ~/.openclaw when empty).
OpenClawScan scan_openclaw(const std::filesystem::path& root);

// Return the list of OpenClaw-style roots (~/.openclaw and ~/.openclaw-old).
std::vector<std::filesystem::path> find_openclaw_roots();

// Archive `src` to `src.parent()/src.name()-archive-YYYYMMDD-HHMMSS`.
// Honours dry_run and returns the destination (or empty on failure).
std::filesystem::path archive_directory(const std::filesystem::path& src,
                                        bool dry_run);

}  // namespace hermes::cli::claw_cmd
