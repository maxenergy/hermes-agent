// C++17 port of hermes_cli/uninstall.py — `hermes uninstall` wizard.
//
// Interactive flow:
//   1. Prompt: keep data vs full vs cancel.
//   2. Confirmation: type 'yes'.
//   3. Stop + uninstall the gateway service (best-effort).
//   4. Scrub PATH additions from common shell rc files.
//   5. Remove the wrapper script under ~/.local/bin or /usr/local/bin.
//   6. Remove the installed code directory.
//   7. When full uninstall, remove $HERMES_HOME.
//
// Helpers below are exposed so tests can verify the PATH-scrub and
// shell-rc discovery against a sandbox directory.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::cli::uninstall_cmd {

// Shell rc candidates under `$HOME` that we inspect for PATH entries.
std::vector<std::filesystem::path> find_shell_configs(
    const std::filesystem::path& home);

// Scrub Hermes PATH entries from the given shell config contents.
// Returns (cleaned_contents, changed_flag).
struct ScrubResult {
    std::string content;
    bool changed = false;
};
ScrubResult scrub_shell_config(const std::string& original);

// Locate wrapper scripts (e.g. ~/.local/bin/hermes).  Returns only
// paths that both exist and look like our wrapper.
std::vector<std::filesystem::path> find_wrapper_scripts(
    const std::filesystem::path& home);

// Whether `content` looks like a Hermes wrapper (contains `hermes_cli`
// or `hermes-agent`).
bool is_hermes_wrapper(const std::string& content);

// Full CLI entry.
int run(int argc, char* argv[]);

}  // namespace hermes::cli::uninstall_cmd
