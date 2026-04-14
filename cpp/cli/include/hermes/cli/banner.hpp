// banner — Startup banner helpers (C++ port of hermes_cli/banner.py).
//
// Exposes pure display helpers used to build the welcome banner shown by
// `hermes` on startup. All heavy rendering (rich.Panel / rich.Table) is
// replaced with plain ANSI text so the banner works in any terminal.
//
// This file contains only testable, side-effect-free helpers plus a
// rendering entry point. Update-check polling lives here too so the CLI
// can schedule it before showing the banner.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace hermes::cli::banner {

// --- ASCII art --------------------------------------------------------------
// The golden caduceus and "HERMES-AGENT" logotype used in the startup banner.
// Kept as constants so tests can assert the line count / width.
extern const std::array<std::string, 6> kHermesLogo;
extern const std::array<std::string, 15> kHermesCaduceus;

// --- Formatting helpers -----------------------------------------------------

// Format a context window size for display, e.g. 128000 → "128K",
// 1048576 → "1M", 524288 → "512K", 1500000 → "1.5M". Rounds to integer
// when the fractional part is < 0.05, otherwise one decimal place.
std::string format_context_length(std::size_t tokens);

// Strip a `_tools` suffix from internal toolset identifiers — used by the
// banner to show `browser` instead of `browser_tools`. Empty input is
// rendered as "unknown" (matches the Python).
std::string display_toolset_name(std::string_view toolset_name);

// Trim a long model name to `max_len` characters, dropping any `...` at the
// very end. Also strips a trailing `.gguf` (local-model filenames).
//
// If the name contains a `/`, only the last segment is considered (so
// `qwen/qwen3-coder-30b-instruct` → `qwen3-coder-30b-instruct`).
std::string short_model_name(std::string_view model, std::size_t max_len = 28);

// Elide a comma-separated tool list so `"names"` never exceeds
// `max_width` characters. When elision occurs the result ends with " ...".
// Input is treated as already-sorted.
std::string elide_csv(const std::vector<std::string>& names,
                      std::size_t max_width);

// --- Skill grouping ---------------------------------------------------------

// Group a list of (name, category) pairs into a category → [names] map,
// preserving insertion order within each category. Unknown / empty
// categories fall back to "general" (matches the Python default).
std::map<std::string, std::vector<std::string>> group_skills_by_category(
    const std::vector<std::pair<std::string, std::string>>& skills);

// Pick up to `max_categories` category labels, sorted alphabetically; any
// overflow is reported via the second return value (how many extras were
// dropped).
struct CategoryPick {
    std::vector<std::string> visible;
    std::size_t remaining = 0;
};
CategoryPick pick_categories(
    const std::map<std::string, std::vector<std::string>>& grouped,
    std::size_t max_categories);

// Build a "+N more" tail for a list of skill names (or tools).
// Always returns the first `max_show` entries + an appended "+k more" token
// when `names.size() > max_show`.
std::string summarise_names(const std::vector<std::string>& names,
                            std::size_t max_show = 8);

// --- Git / update helpers ---------------------------------------------------

struct GitBannerState {
    std::string upstream;       // 8-char short hash of origin/main
    std::string local;          // 8-char short hash of HEAD
    std::size_t ahead = 0;      // commits on HEAD that are not in origin/main
};

// Resolve the git repo that backs the running hermes install. The check
// walks `get_hermes_home()/hermes-agent` first, then falls back to the
// source tree containing the binary. Returns nullopt if neither is a
// git checkout.
std::optional<std::filesystem::path> resolve_repo_dir();

// Get the 8-character short hash for a revision inside a repo. Returns
// nullopt on git failure (missing binary, bad rev, etc.).
std::optional<std::string> git_short_hash(
    const std::filesystem::path& repo_dir,
    const std::string& revision);

// Build the git banner state (upstream / local / ahead). Returns nullopt
// when the repo isn't a git checkout or when git calls fail.
std::optional<GitBannerState> get_git_banner_state(
    const std::optional<std::filesystem::path>& repo_dir = std::nullopt);

// Assemble the title string shown in the banner panel: version, release
// date, and — when available — the upstream/local git state.
std::string format_banner_version_label(const std::string& version,
                                        const std::string& release_date);

// Format the single-line trailer (e.g. "24 tools · 12 skills · 3 MCP
// servers · /help for commands"). Caller supplies the counts; this helper
// just joins them with " · ".
std::string format_summary_line(std::size_t tool_count,
                                std::size_t skill_count,
                                std::size_t mcp_connected,
                                bool include_help_hint = true);

// Format the ⚠ "N commits behind" hint. Empty string when behind == 0.
std::string format_update_warning(std::size_t behind,
                                  const std::string& update_cmd = "hermes update");

// --- Update check cache -----------------------------------------------------

struct UpdateCache {
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::size_t> behind;
};

// Read / write the `~/.hermes/.update_check` JSON cache. Returns nullopt
// on parse error or missing file.
std::optional<UpdateCache> read_update_cache(
    const std::filesystem::path& cache_file);
bool write_update_cache(const std::filesystem::path& cache_file,
                        const UpdateCache& entry);

// Returns true when the cache entry is younger than `max_age`.
bool cache_is_fresh(const UpdateCache& entry,
                    std::chrono::seconds max_age = std::chrono::hours(6));

// --- Rendering entry point --------------------------------------------------

struct BannerContext {
    std::string model;
    std::string cwd;
    std::string session_id;
    std::optional<std::size_t> context_length;
    std::size_t terminal_width = 95;
    std::vector<std::string> toolset_names;            // enabled toolsets
    std::vector<std::pair<std::string, std::string>> tools; // (name, toolset)
    std::vector<std::pair<std::string, std::string>> skills; // (name, category)
    std::size_t mcp_connected = 0;
    std::optional<std::size_t> behind_commits;
    std::string profile_name; // blank or "default" → hidden
    std::string version;
    std::string release_date;
};

// Render the banner to `out` as plain ANSI text. Deterministic — does not
// touch stdout/stderr / env / the filesystem / the network. Used by
// `build_welcome_banner()` and by the banner test suite.
void render_banner(std::ostream& out, const BannerContext& ctx);

// Small convenience — call render_banner(std::cout, ctx).
void print_banner(const BannerContext& ctx);

}  // namespace hermes::cli::banner
