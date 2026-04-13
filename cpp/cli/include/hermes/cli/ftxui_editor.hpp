// FTXUI-style multi-line prompt editor — state machine.
//
// The *state machine* is the testable core: it consumes key events and
// produces a rendered view (a "frame" — a list of display rows plus a
// caret position). The FTXUI binding (off by default) drives this state
// machine from a real terminal; headless tests drive it directly.
//
// Rules this editor respects:
//  - Never emits `\033[K` — callers that render rows must pad to the
//    terminal width with spaces (see render_line_padded()).
//  - History is capped (ring buffer).
//  - Autocomplete candidates are supplied by the caller; the editor owns
//    the selection cursor.
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli {

enum class EditorKey {
    Char,         // data carries one UTF-8 byte
    Enter,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Tab,          // accept current autocomplete candidate (or cycle)
    ShiftTab,     // cycle autocomplete backwards
    Escape,       // dismiss autocomplete
    CtrlC,        // cancel current line (returns "")
    CtrlD,        // EOF when buffer empty
    AltEnter,     // insert literal newline (multi-line)
};

struct EditorEvent {
    EditorKey key = EditorKey::Char;
    char data = 0;  // ASCII byte when key == Char; ignored otherwise.
};

// A provider that returns completions for the given buffer+cursor.
// Implementations should return a stable, sorted list of unique suggestions.
using CompletionProvider =
    std::function<std::vector<std::string>(const std::string& buffer,
                                           std::size_t cursor)>;

struct EditorFrame {
    // One entry per visual row. Rows never contain embedded newlines.
    std::vector<std::string> rows;
    std::size_t cursor_row = 0;
    std::size_t cursor_col = 0;
    // Rendered autocomplete popup, if any (rows already include ' ' padding).
    std::vector<std::string> completions;
    std::size_t selected_completion = 0;  // meaningful iff !completions.empty()
};

class FtxuiEditor {
public:
    FtxuiEditor();

    // --- input ---------------------------------------------------------------
    // Returns true iff the edit resulted in a "submit" (Enter on last line).
    // When a submit happens, buffer() holds the submitted text and buffer is
    // cleared internally (the text is pushed into history first).
    bool handle(const EditorEvent& ev);

    // Convenience: send a run of characters (e.g. pasted text).
    void type(const std::string& text);

    // --- accessors -----------------------------------------------------------
    const std::string& buffer() const { return buffer_; }
    std::size_t cursor() const { return cursor_; }
    const std::deque<std::string>& history() const { return history_; }

    // True if CtrlD was pressed with an empty buffer — caller should exit.
    bool eof() const { return eof_; }

    // --- configuration -------------------------------------------------------
    void set_completion_provider(CompletionProvider cb) {
        completer_ = std::move(cb);
    }
    void set_prompt(std::string p) { prompt_ = std::move(p); }
    void set_terminal_width(std::size_t w) { term_width_ = w; }
    void set_history_limit(std::size_t n) { history_limit_ = n; }

    // --- rendering -----------------------------------------------------------
    EditorFrame render() const;

    // Render a line, padding to term_width_ with spaces (never emits \033[K).
    std::string render_line_padded(const std::string& line) const;

    // Clear state (for tests / /reset).
    void reset();

private:
    // Recompute autocomplete_ from current buffer + cursor.
    void recompute_completions();

    // Submit the current buffer: push to history, clear buffer_.
    std::string commit_submit();

    std::string buffer_;
    std::size_t cursor_ = 0;
    std::string prompt_ = "❯ ";
    std::size_t term_width_ = 80;
    std::size_t history_limit_ = 100;

    std::deque<std::string> history_;
    std::size_t history_index_ = 0;  // 0 == current (not-in-history) buffer
    std::string pending_buffer_;     // saved when browsing history via Up

    CompletionProvider completer_;
    std::vector<std::string> completions_;
    std::size_t completion_index_ = 0;

    bool eof_ = false;
};

}  // namespace hermes::cli
