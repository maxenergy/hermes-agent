// C++17 port of pure-logic helpers from `hermes_cli/skills_hub.py`.
//
// Captures all the state-free computations that drive the `hermes skills`
// subcommand without requiring a live registry / network layer:
//   * trust-rank ordering
//   * category derivation from install path
//   * pagination math
//   * per-source limits lookup
//   * metadata line formatting (mirrors Rich-style markup)
//   * result-entry sorting key.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::skills_hub_helpers {

// ---------------------------------------------------------------------------
// Trust levels.
// ---------------------------------------------------------------------------

// Rank value used for sorting (higher = more trusted).
//   * "builtin"   -> 3
//   * "trusted"   -> 2
//   * "community" -> 1
//   * anything else -> 0
int trust_rank(const std::string& trust_level);

// Rich-style colour style name for a given trust level.
std::string trust_style(const std::string& trust_level);

// Label to display — "official" if source=="official", otherwise trust_level.
std::string trust_label(const std::string& source,
                        const std::string& trust_level);

// ---------------------------------------------------------------------------
// Category derivation.
// ---------------------------------------------------------------------------

// Derive the directory-only component of an install path.
// Mirrors `_derive_category_from_install_path`:
//   * "" / "." -> ""
//   * "tools/foo" -> "tools"
//   * "a/b/c"     -> "a/b"
std::string derive_category_from_install_path(const std::string& install_path);

// ---------------------------------------------------------------------------
// Per-source search limits.
// ---------------------------------------------------------------------------

// Canonical per-source limit lookup (returns 100 for unknown sources).
int per_source_limit(const std::string& source);

// ---------------------------------------------------------------------------
// Pagination.
// ---------------------------------------------------------------------------

struct PageWindow {
    std::size_t page;        // 1-based, clamped
    std::size_t total_pages; // at least 1
    std::size_t start;       // inclusive
    std::size_t end;         // exclusive
    std::size_t page_size;   // clamped to 1..100
};

// Compute pagination window for a list of `total` items.  Clamps page_size
// to [1, 100] and page to [1, total_pages].  Empty list -> total_pages=1.
PageWindow paginate(std::size_t total,
                    std::size_t page,
                    std::size_t page_size);

// ---------------------------------------------------------------------------
// Extra-metadata formatting.
// ---------------------------------------------------------------------------

struct ExtraMetadata {
    std::string repo_url;
    std::string detail_url;
    std::string index_url;
    std::string endpoint;
    std::string install_command;
    std::optional<int> installs;
    std::string weekly_installs;
    // Each entry is (audit-name, status).
    std::vector<std::pair<std::string, std::string>> security_audits;
};

// Return Rich-formatted lines for non-empty fields, in stable order.
std::vector<std::string> format_extra_metadata_lines(
    const ExtraMetadata& extra);

// ---------------------------------------------------------------------------
// Result sorting.
// ---------------------------------------------------------------------------

// Minimal subset of SkillResult for ordering / de-dup.
struct Result {
    std::string name;
    std::string identifier;
    std::string source;
    std::string trust_level;
    std::string description;
};

// Deduplicate by `name`, preferring the entry with higher trust rank.
std::vector<Result> deduplicate_by_name(const std::vector<Result>& results);

// Sort in-place using canonical ordering:
//   1) trust-rank DESC
//   2) source == "official" first
//   3) name ASC (case-insensitive)
void sort_browse_results(std::vector<Result>& results);

// ---------------------------------------------------------------------------
// Short-name resolution logic.
// ---------------------------------------------------------------------------

// Classify the resolution outcome given a list of exact-match results.
enum class ResolveOutcome {
    NoMatch,   // zero results
    Exact,     // exactly one — unambiguous, returns that identifier
    Ambiguous, // multiple matches at the same name
};

ResolveOutcome classify_resolution(const std::vector<Result>& exact_matches);

// ---------------------------------------------------------------------------
// Description truncation (matches Rich table column rendering).
// ---------------------------------------------------------------------------

// Truncate to max_len characters, appending "..." if the original was
// longer.  Mirrors the inline logic in `do_search`.
std::string truncate_description(const std::string& desc, std::size_t max_len);

// ---------------------------------------------------------------------------
// Tap-action validation & source labelling.
// ---------------------------------------------------------------------------

// Canonical tap actions accepted by `hermes skills tap <action>`.
bool is_valid_tap_action(const std::string& action);

// Return a human-friendly label for a source identifier.
//   "official" -> "Nous Research (official)"
//   "skills-sh" -> "skills.sh"
//   "github"    -> "GitHub"
//   "clawhub"   -> "ClawHub"
//   "lobehub"   -> "LobeHub"
//   "well-known" -> ".well-known"
//   "claude-marketplace" -> "Claude Marketplace"
//   anything else -> the input unchanged.
std::string source_label(const std::string& source);

// Return true when the given source filter string is accepted by the
// `--source` flag (either "all" or a registered source name).
bool is_valid_source_filter(const std::string& source);

// ---------------------------------------------------------------------------
// Search-summary line.
// ---------------------------------------------------------------------------

// Build the "Skills Hub — N result(s)" header.
std::string search_header(std::size_t count);

// Build the page-status line.
std::string browse_status_line(std::size_t total,
                               std::size_t page,
                               std::size_t total_pages,
                               const std::string& source_filter,
                               std::size_t timed_out_sources);

}  // namespace hermes::cli::skills_hub_helpers
