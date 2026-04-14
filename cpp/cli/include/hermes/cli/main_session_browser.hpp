// main_session_browser — terminal session picker.
//
// Ports hermes_cli/main.py::_session_browse_picker() and the row-formatting
// helpers used by `hermes sessions browse` / `hermes sessions list`.
//
// The curses rendering itself lives in hermes_cli/curses_ui.cpp — this
// module focuses on the data model, sorting, filtering, and formatting,
// so the logic is testable without a terminal.
#pragma once

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::session_browser {

// ---------------------------------------------------------------------------
// Session summary — matches the SessionDB row shape we read from disk.
// ---------------------------------------------------------------------------
struct Session {
    std::string id;
    std::string title;
    std::string preview;
    std::string source;    // "cli" | "tool" | "gateway" | "acp" | …
    std::time_t last_active = 0;
    std::time_t created_at  = 0;
    std::size_t message_count = 0;
};

// Load all sessions from <HERMES_HOME>/sessions/*.json.  Sorted most-recent
// first.  `source_filter` is optional — pass empty to include all.
std::vector<Session> load_sessions(
    const std::string& source_filter = "",
    std::size_t max_rows = 500);

// ---------------------------------------------------------------------------
// Filtering / sorting
// ---------------------------------------------------------------------------

// Case-insensitive substring match across title, preview, id, source.
bool session_matches_query(const Session& s, const std::string& query);

// Filter `sessions` by `query`, returning a vector of *pointers* to the
// matched rows in input order.  Pointers are valid as long as the input
// vector is unchanged.
std::vector<const Session*> filter_sessions(
    const std::vector<Session>& sessions,
    const std::string& query);

// Sort sessions by last-active descending.  Stable.
void sort_sessions_by_recency(std::vector<Session>& sessions);

// Sort by title (case-insensitive) ascending.
void sort_sessions_by_title(std::vector<Session>& sessions);

// ---------------------------------------------------------------------------
// Row formatting — mirrors _format_row() in the Python picker.
// ---------------------------------------------------------------------------
struct RowLayout {
    int arrow_width    = 3;
    int title_width    = 40;
    int active_width   = 12;
    int source_width   = 6;
    int id_width       = 18;
    int terminal_width = 80;
};

// Compute the layout for a given terminal width.  Returns the adjusted
// RowLayout where title_width absorbs any slack.
RowLayout layout_for_width(int terminal_width);

// Format one row.  `selected` toggles the leading arrow indicator.
std::string format_row(const Session& s, const RowLayout& layout,
                       bool selected = false);

// Format the column header row.
std::string format_column_header(const RowLayout& layout);

// Format a compact one-line row — used by `hermes sessions list`.
std::string format_list_row(const Session& s);

// ---------------------------------------------------------------------------
// Text-mode picker (no curses) — writes rows to stdout and reads the user's
// index from stdin.  Returns the chosen session id, or empty on cancel.
// Exposed so tests can drive the flow with stringstreams.
// ---------------------------------------------------------------------------
struct PickerOptions {
    int page_size = 20;
    bool show_index = true;
    std::string prompt = "Select session [number / q to quit]: ";
};

struct PickerIO {
    std::istream* in = nullptr;
    std::ostream* out = nullptr;
};

std::optional<std::string> text_picker(const std::vector<Session>& sessions,
                                       const PickerOptions& opts,
                                       PickerIO io);

// Convenience — uses std::cin / std::cout.
std::optional<std::string> text_picker(const std::vector<Session>& sessions,
                                       const PickerOptions& opts = {});

// ---------------------------------------------------------------------------
// High-level entry — `hermes sessions browse`.  Tries curses first, falls
// back to text_picker on terminals that don't support it.  Returns 0 on
// success, 1 on error.  On success, `selected` is set to the chosen id
// (or empty string if the user cancelled).
// ---------------------------------------------------------------------------
int run_browse(std::string* selected = nullptr);

// Emit a listing.  Honors --limit / --source / --json via opts.
struct ListOptions {
    std::size_t limit = 20;
    std::string source_filter;
    std::string since_expr;     // "24h", "7d", …
    bool as_json = false;
};
int run_list(const ListOptions& opts, std::ostream& out);

}  // namespace hermes::cli::session_browser
