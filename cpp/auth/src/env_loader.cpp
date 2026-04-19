#include "hermes/auth/env_loader.hpp"

#include "hermes/core/env.hpp"
#include "hermes/core/path.hpp"

#include <filesystem>
#include <system_error>

namespace hermes::auth {

namespace fs = std::filesystem;

void load_profile_env() {
    // Layer 1: `<HERMES_HOME>/.env` — per-profile credentials.  This
    // MUST win over any project-root `.env` so tools that run outside
    // `hermes_cli` (e.g. trajectory_compressor) still pick up the
    // user's active profile instead of stale checkout credentials.
    // Upstream parity: commit 4b47856f.  `get_hermes_home()` reads
    // `std::getenv("HERMES_HOME")` and falls back to `$HOME/.hermes`.
    const auto home_env = hermes::core::path::get_hermes_home() / ".env";
    std::error_code ec;
    if (fs::exists(home_env, ec) && !ec) {
        hermes::core::env::load_dotenv(home_env);
    }

    // Layer 2: `./.env` in the CWD — project-local fallback.  Skip if
    // it happens to be the same file as the home env (identical inode)
    // to avoid doing redundant work.  `load_dotenv` itself refuses to
    // clobber already-set env vars so the HERMES_HOME layer always
    // wins on overlapping keys.
    const auto cwd_env = fs::current_path(ec) / ".env";
    if (ec) {
        return;
    }
    if (fs::exists(cwd_env, ec) && !ec) {
        if (fs::equivalent(home_env, cwd_env, ec) && !ec) {
            return;  // Same file — don't load twice.
        }
        ec.clear();
        hermes::core::env::load_dotenv(cwd_env);
    }
}

}  // namespace hermes::auth
