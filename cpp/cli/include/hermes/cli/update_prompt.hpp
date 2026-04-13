// Update-availability prompt for the hermes CLI.
//
// On startup the CLI calls `maybe_prompt_update()` once.  Behaviour:
//   1. Skip silently when stdin is not a TTY, when --no-update-check
//      was passed, or when the env says we are in CI (CI=true).
//   2. Read the throttle file at
//      `${HERMES_HOME}/state/update.json`.  If the cached check is
//      < 24h old, skip.
//   3. Fetch a JSON manifest (default path is the GitHub-releases
//      `release/latest.json` — pluggable via `UpdateConfig::manifest`).
//   4. Compare semver-ish versions; if the remote is newer, print
//      `Update from vX -> vY? [y/N]` and read a single line from stdin.
//   5. Persist `last_update_check_at` regardless of the user's answer
//      so the prompt only fires once per 24h.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace hermes::cli::update_prompt {

struct UpdateConfig {
    // Local CLI version (e.g. "0.1.0").  Compared against
    // manifest.latest_version.
    std::string current_version;
    // Pluggable fetcher.  Receives the configured manifest URL and
    // returns the raw JSON body, or std::nullopt on transport failure.
    // Default impl uses libcurl when linked; the real injection point
    // is in tests so we never reach the network during CI.
    std::function<std::optional<std::string>(const std::string&)> fetch;
    // Pluggable input/output for the prompt.  When unset, the impl
    // uses std::cin / std::cout.
    std::function<std::optional<std::string>()> read_line;
    std::function<void(std::string_view)> write_line;
    // Override the throttle state path (defaults to
    // ${HERMES_HOME}/state/update.json via path::get_hermes_home()).
    std::filesystem::path state_path_override;
    // Override the manifest URL.
    std::string manifest_url =
        "https://raw.githubusercontent.com/hermes-agent/release/latest.json";
    // Force-bypass the TTY/CI/--no-update-check skip gates (used by
    // tests).  The 24h throttle is still honoured — set
    // `bypass_throttle = true` to ignore that as well.
    bool force = false;
    bool bypass_throttle = false;
    // Honour --no-update-check.
    bool no_update_check_flag = false;
    // Throttle window.
    std::chrono::seconds throttle = std::chrono::hours(24);
    // Inject "now" for deterministic tests.
    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now();
};

// Result of `maybe_prompt_update()`.
struct UpdateOutcome {
    bool prompted = false;        // we displayed the prompt
    bool user_accepted = false;   // user typed y / yes
    bool throttled = false;       // skipped due to recent check
    bool skipped_no_tty = false;  // not a TTY (or CI)
    std::optional<std::string> latest_version;
    std::string detail;
};

// Parse `release/latest.json` of the form
// {"version": "0.2.0", "url": "...", "notes": "..."}.
// Returns std::nullopt if the JSON is unparseable / missing version.
struct LatestManifest {
    std::string version;
    std::string url;
    std::string notes;
};
std::optional<LatestManifest> parse_latest_manifest(std::string_view body);

// Pure semver-ish comparison: returns true when `remote` is strictly
// newer than `local`.  Accepts "X.Y.Z" with optional leading "v".
// Trailing pre-release suffixes ("-rc1") sort *before* the bare
// release.  Falls back to lexicographic on parse failure.
bool is_newer_version(std::string_view local, std::string_view remote);

// Run the throttle gate without doing the prompt — useful for tests
// and for the daemon to force-mark a check as having happened.
struct ThrottleState {
    std::chrono::system_clock::time_point last_check{};
    std::string last_seen_version;
    bool exists = false;
};
ThrottleState load_throttle(const std::filesystem::path& state_path);
void save_throttle(const std::filesystem::path& state_path,
                   const ThrottleState& s);

// Default state path resolution.  Public so tests can mirror it.
std::filesystem::path default_state_path();

// Main entrypoint.  Safe to call from a signal-free context only.
UpdateOutcome maybe_prompt_update(UpdateConfig cfg);

}  // namespace hermes::cli::update_prompt
