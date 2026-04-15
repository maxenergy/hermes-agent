// Pure CLI presentation helpers ported from agent/display.py.
//
// This C++ port covers the portable parts of display.py:
//   * Tool-call preview formatter (build_tool_preview)
//   * Cute one-line tool completion message (get_cute_tool_message)
//   * Local edit snapshot + unified-diff renderer (capture/diff/extract/render)
//   * Context-pressure progress lines (format_context_pressure[_gateway])
//   * Kawaii face / thinking verb / spinner frame data tables
//   * Tool-failure detector
//
// Everything that depends on a live TTY, threads, or prompt_toolkit
// (KawaiiSpinner.start/stop/_animate) is intentionally NOT ported — those
// are CLI-runtime concerns. The data tables and message builders are.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent::display {

// ANSI escape used to reset attributes after a coloured run.
inline constexpr const char* kAnsiReset = "\033[0m";

// ----------------------------------------------------------------------------
// Tool preview length (0 = unlimited).  Mirrors the module-global in display.py.
// ----------------------------------------------------------------------------
void set_tool_preview_max_len(int n) noexcept;
int  get_tool_preview_max_len() noexcept;

// Collapse all runs of whitespace (incl. newlines) into single spaces.
std::string oneline(const std::string& text);

// Build a one-line preview of a tool call's primary argument.  Returns nullopt
// when no useful preview can be produced (e.g. unknown tool with empty args).
//
// `max_len <= 0` means use the global preview length set via
// set_tool_preview_max_len().  `max_len > 0` truncates with "...".
std::optional<std::string> build_tool_preview(
    const std::string& tool_name,
    const nlohmann::json& args,
    int max_len = -1);

// ----------------------------------------------------------------------------
// Tool-failure detection
// ----------------------------------------------------------------------------
struct FailureInfo {
    bool is_failure = false;
    std::string suffix;  // e.g. " [exit 1]", " [error]", " [full]", or ""
};
FailureInfo detect_tool_failure(
    const std::string& tool_name,
    const std::optional<std::string>& result);

// Build the "cute" one-line tool completion message that replaces the spinner.
//
// Format: "┊ {emoji} {verb:9} {detail}  {duration}".  Failed tools get the
// failure suffix appended.  No skin-engine integration in C++ — the prefix
// stays as "┊".  `result` is optional — when present, it's scanned for failure
// markers.
std::string get_cute_tool_message(
    const std::string& tool_name,
    const nlohmann::json& args,
    double duration_seconds,
    const std::optional<std::string>& result = std::nullopt);

// ----------------------------------------------------------------------------
// Local edit snapshot (file-content capture for inline diff display)
// ----------------------------------------------------------------------------
struct LocalEditSnapshot {
    std::vector<std::filesystem::path> paths;
    // path-as-string -> file content, or nullopt for missing/unreadable files.
    std::map<std::string, std::optional<std::string>> before;
};

// Resolve the filesystem targets a write-capable tool would touch.  Pure;
// no I/O.  Returns the empty vector when the tool isn't write-capable or
// no path argument is present.
std::vector<std::filesystem::path> resolve_local_edit_paths(
    const std::string& tool_name,
    const nlohmann::json& function_args);

// Capture the before-state of every path that the given tool call would
// rewrite.  Reads files (UTF-8 text only).  Returns nullopt when no
// applicable paths were produced.
std::optional<LocalEditSnapshot> capture_local_edit_snapshot(
    const std::string& tool_name,
    const nlohmann::json& function_args);

// Conservatively decide whether a tool result string represents success.
bool result_succeeded(const std::optional<std::string>& result);

// Generate a unified diff describing changes between snapshot.before and the
// current on-disk contents.  Returns nullopt when nothing changed.
std::optional<std::string> diff_from_snapshot(
    const std::optional<LocalEditSnapshot>& snapshot);

// Extract a unified diff for an edit tool's result.  For the "patch" tool
// the diff is read from the result JSON's "diff" field; for write_file /
// skill_manage we diff the captured snapshot.
std::optional<std::string> extract_edit_diff(
    const std::string& tool_name,
    const std::optional<std::string>& result,
    const nlohmann::json& function_args = {},
    const std::optional<LocalEditSnapshot>& snapshot = std::nullopt);

// Split a unified diff into per-file sections at "--- " boundaries.
std::vector<std::string> split_unified_diff_sections(const std::string& diff);

// Render a single diff section into ANSI-coloured display lines using the
// fallback dark-terminal colour palette.  Lines have no trailing newline.
std::vector<std::string> render_inline_unified_diff(const std::string& diff);

// Render multiple diff sections, capping the file count and total line count.
// Adds an "… omitted N diff line(s) across M additional file(s)/section(s)"
// summary line when truncation occurs.
std::vector<std::string> summarize_rendered_diff_sections(
    const std::string& diff,
    int max_files = 6,
    int max_lines = 80);

// ----------------------------------------------------------------------------
// Context pressure formatting
// ----------------------------------------------------------------------------
// Returns a single ANSI-coloured line summarising compaction progress for the
// CLI.  `compaction_progress` is in [0.0, 1.0]; values >= 1.0 mean the
// compactor is about to fire.
std::string format_context_pressure(
    double compaction_progress,
    long long threshold_tokens,
    double threshold_percent,
    bool compression_enabled = true);

// Plain-text variant for messaging gateways (Telegram/Discord/etc).
std::string format_context_pressure_gateway(
    double compaction_progress,
    double threshold_percent,
    bool compression_enabled = true);

// ----------------------------------------------------------------------------
// Kawaii face + spinner frame data tables (for spinner / skin code)
// ----------------------------------------------------------------------------
const std::vector<std::string>& kawaii_waiting_faces();
const std::vector<std::string>& kawaii_thinking_faces();
const std::vector<std::string>& thinking_verbs();
const std::vector<std::string>& spinner_frames(const std::string& kind);
std::vector<std::string> spinner_kinds();

}  // namespace hermes::agent::display
