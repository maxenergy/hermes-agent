#include "hermes/cli/curses_ui.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

namespace hermes::cli::curses_ui {

namespace {

std::string pad_to(const std::string& s, std::size_t w) {
    if (s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}

}  // namespace

// ----- MenuState ------------------------------------------------------------

void MenuState::move_up() {
    if (items.empty()) return;
    // Skip disabled items.
    for (std::size_t step = 0; step < items.size(); ++step) {
        if (selected == 0) {
            selected = items.size() - 1;
        } else {
            --selected;
        }
        if (items[selected].enabled) break;
    }
    if (selected < scroll) scroll = selected;
}

void MenuState::move_down() {
    if (items.empty()) return;
    for (std::size_t step = 0; step < items.size(); ++step) {
        selected = (selected + 1) % items.size();
        if (items[selected].enabled) break;
    }
    if (selected >= scroll + viewport_rows) {
        scroll = selected - viewport_rows + 1;
    }
}

void MenuState::page_up() {
    if (items.empty()) return;
    if (selected >= viewport_rows) selected -= viewport_rows;
    else selected = 0;
    scroll = (selected >= viewport_rows) ? selected - viewport_rows + 1 : 0;
}

void MenuState::page_down() {
    if (items.empty()) return;
    selected = std::min(selected + viewport_rows, items.size() - 1);
    if (selected >= scroll + viewport_rows) {
        scroll = selected - viewport_rows + 1;
    }
}

void MenuState::select_current() {
    if (items.empty()) return;
    if (!items[selected].enabled) return;
    done = true;
}

void MenuState::cancel() {
    cancelled = true;
    done = true;
}

std::vector<std::string> MenuState::render(std::size_t width) const {
    std::vector<std::string> out;
    if (!title.empty()) {
        out.push_back(pad_to(title, width));
        out.push_back(pad_to(std::string(std::min<std::size_t>(width, title.size()), '-'), width));
    }
    const std::size_t end = std::min(scroll + viewport_rows, items.size());
    for (std::size_t i = scroll; i < end; ++i) {
        const auto& it = items[i];
        std::string marker = (i == selected) ? "> " : "  ";
        std::string disabled_mark = it.enabled ? "" : " (disabled)";
        std::string line = marker + it.label;
        if (!it.badge.empty()) line += "  " + it.badge;
        line += disabled_mark;
        out.push_back(pad_to(line, width));
    }
    return out;
}

// ----- TableState -----------------------------------------------------------

std::vector<std::size_t> TableState::column_widths(std::size_t max_width) const {
    if (headers.empty() && rows.empty()) return {};
    std::size_t ncols = headers.size();
    for (const auto& r : rows) ncols = std::max(ncols, r.cells.size());
    std::vector<std::size_t> widths(ncols, 0);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        widths[i] = std::max(widths[i], headers[i].size());
    }
    for (const auto& r : rows) {
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            widths[i] = std::max(widths[i], r.cells[i].size());
        }
    }
    // Clip: if total > max_width, evenly shrink widest columns.
    std::size_t total = 0;
    for (auto w : widths) total += w + 2;  // +2 for separator
    while (total > max_width && ncols > 0) {
        // Reduce the widest column by 1.
        auto it = std::max_element(widths.begin(), widths.end());
        if (*it <= 3) break;
        --(*it);
        --total;
    }
    return widths;
}

std::vector<std::string> TableState::render(std::size_t width) const {
    std::vector<std::string> out;
    auto widths = column_widths(width);

    auto render_row = [&](const std::vector<std::string>& cells) {
        std::string line;
        for (std::size_t i = 0; i < widths.size(); ++i) {
            std::string cell = (i < cells.size()) ? cells[i] : "";
            if (cell.size() > widths[i]) cell = cell.substr(0, widths[i]);
            line += pad_to(cell, widths[i]);
            if (i + 1 < widths.size()) line += "  ";
        }
        return pad_to(line, width);
    };

    if (!headers.empty()) out.push_back(render_row(headers));
    std::size_t sep_total = 0;
    for (auto w : widths) sep_total += w + 2;
    out.push_back(pad_to(std::string(std::min(width, sep_total), '-'), width));

    const std::size_t end = std::min(scroll + viewport_rows, rows.size());
    for (std::size_t i = scroll; i < end; ++i) {
        std::string prefix = (i == selected) ? "> " : "  ";
        auto row = render_row(rows[i].cells);
        out.push_back(pad_to(prefix + row, width));
    }
    return out;
}

// ----- Runners --------------------------------------------------------------

std::optional<std::size_t> run_menu_plaintext(MenuState& state,
                                              std::ostream& out,
                                              std::istream& in) {
    for (const auto& line : state.render(80)) out << line << "\n";
    out << "Enter selection [0-" << (state.items.empty() ? 0 : state.items.size() - 1)
        << "] or 'q' to cancel: ";
    out.flush();
    std::string input;
    if (!std::getline(in, input)) {
        state.cancel();
        return std::nullopt;
    }
    if (input == "q" || input == "Q") {
        state.cancel();
        return std::nullopt;
    }
    try {
        const int i = std::stoi(input);
        if (i < 0 || static_cast<std::size_t>(i) >= state.items.size()) {
            state.cancel();
            return std::nullopt;
        }
        if (!state.items[i].enabled) {
            state.cancel();
            return std::nullopt;
        }
        state.selected = static_cast<std::size_t>(i);
        state.done = true;
        return state.selected;
    } catch (const std::exception&) {
        state.cancel();
        return std::nullopt;
    }
}

std::optional<std::size_t> run_menu(MenuState& state) {
    // No ncurses dependency in this scaffolding — always use the plaintext
    // fallback. A future patch may swap in ncurses-driven rendering; the
    // state machine above is already ready for it.
    return run_menu_plaintext(state, std::cout, std::cin);
}

void render_table_plaintext(const TableState& state,
                            std::ostream& out,
                            std::size_t width) {
    for (const auto& line : state.render(width)) out << line << "\n";
}

}  // namespace hermes::cli::curses_ui
