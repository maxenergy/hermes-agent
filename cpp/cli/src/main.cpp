#include "hermes/cli/main_entry.hpp"
#include "hermes/profile/profile.hpp"

#include <clocale>

int main(int argc, char* argv[]) {
    // Inherit the user's locale (e.g. en_US.UTF-8 / zh_CN.UTF-8) so
    // libc, iostreams and GNU readline treat input as UTF-8 multibyte
    // rather than ASCII bytes.  Without this, CJK characters in the
    // REPL cannot be deleted correctly — backspace removes one byte
    // of a 3-byte rune and leaves a phantom block on screen.
    std::setlocale(LC_ALL, "");

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
