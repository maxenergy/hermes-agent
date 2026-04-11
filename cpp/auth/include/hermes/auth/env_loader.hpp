// Load `.env` files into the process environment.
//
// Two-layer overlay (user overrides project):
//   1. `<HERMES_HOME>/.env` — per-profile credentials written by the
//      setup wizard.
//   2. `./.env` in the CWD — project-local fallback for developers.
//
// Both loads are delegated to `hermes::core::env::load_dotenv`, which
// refuses to clobber variables that are already set in the process
// environment — so the shell-exported value always wins.
#pragma once

namespace hermes::auth {

// Idempotent — safe to call multiple times per process.
void load_profile_env();

}  // namespace hermes::auth
