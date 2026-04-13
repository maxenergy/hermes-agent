#include "hermes/cli/ftxui_editor.hpp"

#include <algorithm>

namespace hermes::cli {

FtxuiEditor::FtxuiEditor() = default;

void FtxuiEditor::type(const std::string& text) {
    for (char c : text) {
        EditorEvent ev;
        ev.key = EditorKey::Char;
        ev.data = c;
        handle(ev);
    }
}

bool FtxuiEditor::handle(const EditorEvent& ev) {
    // Any edit other than Tab/ShiftTab/Escape invalidates the completion cursor.
    auto invalidate_completions = [this] {
        completions_.clear();
        completion_index_ = 0;
    };

    switch (ev.key) {
        case EditorKey::Char: {
            buffer_.insert(cursor_, 1, ev.data);
            ++cursor_;
            recompute_completions();
            return false;
        }
        case EditorKey::Backspace: {
            if (cursor_ > 0) {
                buffer_.erase(cursor_ - 1, 1);
                --cursor_;
            }
            recompute_completions();
            return false;
        }
        case EditorKey::Delete: {
            if (cursor_ < buffer_.size()) buffer_.erase(cursor_, 1);
            recompute_completions();
            return false;
        }
        case EditorKey::Left: {
            if (cursor_ > 0) --cursor_;
            invalidate_completions();
            return false;
        }
        case EditorKey::Right: {
            if (cursor_ < buffer_.size()) ++cursor_;
            invalidate_completions();
            return false;
        }
        case EditorKey::Home: {
            // Move to the start of the current line.
            auto nl = buffer_.rfind('\n', cursor_ == 0 ? 0 : cursor_ - 1);
            cursor_ = (nl == std::string::npos) ? 0 : nl + 1;
            invalidate_completions();
            return false;
        }
        case EditorKey::End: {
            auto nl = buffer_.find('\n', cursor_);
            cursor_ = (nl == std::string::npos) ? buffer_.size() : nl;
            invalidate_completions();
            return false;
        }
        case EditorKey::Up: {
            // If already browsing history, go further back. Otherwise start
            // at newest and save current buffer.
            if (history_.empty()) return false;
            if (history_index_ == 0) pending_buffer_ = buffer_;
            if (history_index_ < history_.size()) {
                ++history_index_;
                buffer_ = history_[history_.size() - history_index_];
                cursor_ = buffer_.size();
            }
            invalidate_completions();
            return false;
        }
        case EditorKey::Down: {
            if (history_index_ == 0) return false;
            --history_index_;
            if (history_index_ == 0) {
                buffer_ = pending_buffer_;
                pending_buffer_.clear();
            } else {
                buffer_ = history_[history_.size() - history_index_];
            }
            cursor_ = buffer_.size();
            invalidate_completions();
            return false;
        }
        case EditorKey::Tab: {
            if (completions_.empty()) {
                recompute_completions();
                if (completions_.empty()) return false;
            } else {
                completion_index_ =
                    (completion_index_ + 1) % completions_.size();
            }
            return false;
        }
        case EditorKey::ShiftTab: {
            if (completions_.empty()) return false;
            if (completion_index_ == 0) {
                completion_index_ = completions_.size() - 1;
            } else {
                --completion_index_;
            }
            return false;
        }
        case EditorKey::Escape: {
            invalidate_completions();
            return false;
        }
        case EditorKey::CtrlC: {
            buffer_.clear();
            cursor_ = 0;
            history_index_ = 0;
            pending_buffer_.clear();
            invalidate_completions();
            return false;
        }
        case EditorKey::CtrlD: {
            if (buffer_.empty()) {
                eof_ = true;
            } else {
                // Same as Delete when buffer is non-empty.
                if (cursor_ < buffer_.size()) buffer_.erase(cursor_, 1);
                recompute_completions();
            }
            return false;
        }
        case EditorKey::AltEnter: {
            buffer_.insert(cursor_, 1, '\n');
            ++cursor_;
            invalidate_completions();
            return false;
        }
        case EditorKey::Enter: {
            // If the user has selected a completion, accept it instead of
            // submitting — matches prompt_toolkit behavior.
            if (!completions_.empty() &&
                completion_index_ < completions_.size()) {
                // Replace the current word with the selected completion.
                // Word = contiguous non-space prefix ending at cursor_.
                std::size_t start = cursor_;
                while (start > 0 && !std::isspace(static_cast<unsigned char>(
                                        buffer_[start - 1]))) {
                    --start;
                }
                buffer_.replace(start, cursor_ - start,
                                completions_[completion_index_]);
                cursor_ = start + completions_[completion_index_].size();
                invalidate_completions();
                return false;
            }
            commit_submit();
            return true;
        }
    }
    return false;
}

std::string FtxuiEditor::commit_submit() {
    std::string out = buffer_;
    if (!out.empty()) {
        if (history_.empty() || history_.back() != out) {
            history_.push_back(out);
            while (history_.size() > history_limit_) history_.pop_front();
        }
    }
    buffer_.clear();
    cursor_ = 0;
    history_index_ = 0;
    pending_buffer_.clear();
    completions_.clear();
    completion_index_ = 0;
    return out;
}

void FtxuiEditor::recompute_completions() {
    completions_.clear();
    completion_index_ = 0;
    if (completer_) {
        completions_ = completer_(buffer_, cursor_);
    }
}

EditorFrame FtxuiEditor::render() const {
    EditorFrame f;
    // Split buffer on newlines.
    std::string line;
    std::size_t row = 0;
    std::size_t col = 0;
    std::size_t seen = 0;
    for (char c : buffer_) {
        if (c == '\n') {
            f.rows.push_back(line);
            line.clear();
            if (seen < cursor_) {
                ++row;
                col = 0;
            }
        } else {
            line.push_back(c);
            if (seen < cursor_) ++col;
        }
        ++seen;
    }
    f.rows.push_back(line);
    f.cursor_row = row;
    f.cursor_col = col;

    if (f.rows.empty()) f.rows.emplace_back();
    // Prefix the first row with the prompt (callers can strip if needed).
    f.rows[0] = prompt_ + f.rows[0];
    f.cursor_col += prompt_.size();

    // Pad every row to term_width_ with spaces (no \033[K).
    for (auto& r : f.rows) r = render_line_padded(r);

    if (!completions_.empty()) {
        f.selected_completion = completion_index_;
        for (std::size_t i = 0; i < completions_.size(); ++i) {
            std::string row_text = (i == completion_index_ ? "> " : "  ") +
                                   completions_[i];
            f.completions.push_back(render_line_padded(row_text));
        }
    }
    return f;
}

std::string FtxuiEditor::render_line_padded(const std::string& line) const {
    if (line.size() >= term_width_) return line;
    return line + std::string(term_width_ - line.size(), ' ');
}

void FtxuiEditor::reset() {
    buffer_.clear();
    cursor_ = 0;
    history_.clear();
    history_index_ = 0;
    pending_buffer_.clear();
    completions_.clear();
    completion_index_ = 0;
    eof_ = false;
}

}  // namespace hermes::cli
