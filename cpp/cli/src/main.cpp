#include "hermes/cli/main_entry.hpp"
#include "hermes/profile/profile.hpp"

int main(int argc, char* argv[]) {
    // CRITICAL: pre-parse `--profile=NAME` / `--profile NAME` / `-p NAME`
    // and apply it BEFORE main_entry() — and therefore before any code
    // that reads HERMES_HOME via `hermes::core::path::get_hermes_home()`.
    // Mirrors Python's `_apply_profile_override()` in hermes_cli/main.py
    // which sets HERMES_HOME before any module import.  The matched
    // tokens are stripped from argv so downstream parsers see a clean
    // argument slice.
    if (auto profile = hermes::profile::preparse_profile_argv(argc, argv);
        profile.has_value()) {
        hermes::profile::apply_profile_override(std::move(profile));
    }
    return hermes::cli::main_entry(argc, argv);
}
