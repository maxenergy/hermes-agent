// curses_ui — TUI helpers for `hermes tools`, `hermes skills`, etc.
//
// This is the C++ equivalent of hermes_cli/curses_ui.py. It exposes a
// headless-testable state machine (MenuState, TableState) plus a thin
// ncurses renderer. Callers that don't have a TTY (CI, pipe mode) use the
// non-ncurses fallbacks (select_menu_fallback, render_table_plaintext).
//
// Pitfalls we avoid:
//  - never emit `\033[K` — we pad with spaces to wipe old rows.
//  - never use simple_term_menu-style scroll ghosts — each frame is a
//    complete redraw with padding.
#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::curses_ui {

// --------------------------------------------------------------------------
// Menu: single-selection list with keyboard navigation.
// --------------------------------------------------------------------------
struct MenuItem {
    std::string label;
    std::string value;       // returned when selected; defaults to label
    bool enabled = true;
    std::string badge;       // e.g. "[on]" / "[off]"
};

struct MenuState {
    std::vector<MenuItem> items;
    std::size_t selected = 0;
    std::size_t scroll = 0;
    std::size_t viewport_rows = 10;
    std::string title;
    bool done = false;
    bool cancelled = false;

    void move_up();
    void move_down();
    void page_up();
    void page_down();
    void select_current();
    void cancel();

    // Render to plain text (one line per item, full-width-padded).
    std::vector<std::string> render(std::size_t width) const;
};

// --------------------------------------------------------------------------
// Table: multi-column display with optional inline toggles.
// --------------------------------------------------------------------------
struct TableRow {
    std::vector<std::string> cells;
    // Optional key used by callers to key state back to a row.
    std::string key;
};

struct TableState {
    std::vector<std::string> headers;
    std::vector<TableRow> rows;
    std::size_t selected = 0;
    std::size_t scroll = 0;
    std::size_t viewport_rows = 15;
    bool done = false;

    // Compute per-column widths from header + rows, clipped to `max_width`.
    std::vector<std::size_t> column_widths(std::size_t max_width) const;

    // Render the table (headers + rows) as padded text.
    std::vector<std::string> render(std::size_t width) const;
};

// --------------------------------------------------------------------------
// ncurses renderers (no-op stubs on systems without ncurses).
// --------------------------------------------------------------------------

// Run an interactive menu. Returns the selected index, or nullopt if the
// user cancelled. Writes nothing on stdout on cancel.
std::optional<std::size_t> run_menu(MenuState& state);

// Plain-text fallback for the menu (prints to `out`, reads from `in`).
// Used when stdin isn't a TTY.
std::optional<std::size_t> run_menu_plaintext(MenuState& state,
                                              std::ostream& out,
                                              std::istream& in);

// Dump a table as plain text (used by `hermes tools list --no-interactive`).
void render_table_plaintext(const TableState& state,
                            std::ostream& out,
                            std::size_t width = 80);

}  // namespace hermes::cli::curses_ui
