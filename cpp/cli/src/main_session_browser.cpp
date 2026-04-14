// main_session_browser — port of the Python session picker data model.
// Curses rendering is deferred to existing curses_ui.cpp; this file owns
// the sort / filter / format / text-picker logic.

#include "hermes/cli/main_session_browser.hpp"
#include "hermes/cli/main_preparse.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::cli::session_browser {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string truncate(const std::string& s, std::size_t width) {
    if (s.size() <= width) return s;
    if (width == 0) return {};
    if (width == 1) return ".";
    return s.substr(0, width - 1) + std::string("…");
}

std::string pad(const std::string& s, std::size_t width) {
    std::string t = truncate(s, width);
    if (t.size() < width) t += std::string(width - t.size(), ' ');
    return t;
}

// Parse a "24h" / "7d" / "30m" duration expression to a count of seconds.
std::int64_t parse_duration(const std::string& expr) {
    if (expr.empty()) return 0;
    std::int64_t num = 0;
    std::size_t i = 0;
    while (i < expr.size() && std::isdigit(static_cast<unsigned char>(expr[i]))) {
        num = num * 10 + (expr[i] - '0');
        ++i;
    }
    if (i == 0 || i == expr.size()) return num;
    char unit = expr[i];
    switch (unit) {
        case 's': return num;
        case 'm': return num * 60;
        case 'h': return num * 3600;
        case 'd': return num * 86400;
        case 'w': return num * 604800;
        default: return num;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------
std::vector<Session> load_sessions(const std::string& source_filter,
                                   std::size_t max_rows) {
    std::vector<Session> out;
    std::error_code ec;
    fs::path dir;
    try {
        dir = hermes::core::path::get_hermes_home() / "sessions";
    } catch (...) {
        return out;
    }
    if (!fs::is_directory(dir, ec)) return out;

    std::vector<fs::directory_entry> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".json") continue;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  std::error_code c;
                  return fs::last_write_time(a.path(), c) >
                         fs::last_write_time(b.path(), c);
              });

    for (const auto& e : entries) {
        if (out.size() >= max_rows) break;
        try {
            std::ifstream f(e.path());
            nlohmann::json j;
            f >> j;
            Session s;
            s.id      = j.value("id", e.path().stem().string());
            s.title   = j.value("title", "");
            s.preview = j.value("preview", "");
            s.source  = j.value("source", "cli");
            if (j.contains("last_active") && j["last_active"].is_number()) {
                s.last_active =
                    static_cast<std::time_t>(j["last_active"].get<double>());
            }
            if (j.contains("created_at") && j["created_at"].is_number()) {
                s.created_at =
                    static_cast<std::time_t>(j["created_at"].get<double>());
            }
            if (j.contains("message_count") && j["message_count"].is_number()) {
                s.message_count =
                    static_cast<std::size_t>(j["message_count"].get<double>());
            }
            if (!source_filter.empty() && s.source != source_filter) continue;
            out.push_back(std::move(s));
        } catch (...) {
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Filtering / sorting
// ---------------------------------------------------------------------------
bool session_matches_query(const Session& s, const std::string& query) {
    if (query.empty()) return true;
    auto q = to_lower(query);
    if (to_lower(s.title).find(q)   != std::string::npos) return true;
    if (to_lower(s.preview).find(q) != std::string::npos) return true;
    if (to_lower(s.id).find(q)      != std::string::npos) return true;
    if (to_lower(s.source).find(q)  != std::string::npos) return true;
    return false;
}

std::vector<const Session*> filter_sessions(
    const std::vector<Session>& sessions, const std::string& query) {
    std::vector<const Session*> out;
    out.reserve(sessions.size());
    for (const auto& s : sessions) {
        if (session_matches_query(s, query)) out.push_back(&s);
    }
    return out;
}

void sort_sessions_by_recency(std::vector<Session>& sessions) {
    std::stable_sort(sessions.begin(), sessions.end(),
                     [](const Session& a, const Session& b) {
                         return a.last_active > b.last_active;
                     });
}

void sort_sessions_by_title(std::vector<Session>& sessions) {
    std::stable_sort(sessions.begin(), sessions.end(),
                     [](const Session& a, const Session& b) {
                         return to_lower(a.title) < to_lower(b.title);
                     });
}

// ---------------------------------------------------------------------------
// Row formatting
// ---------------------------------------------------------------------------
RowLayout layout_for_width(int terminal_width) {
    RowLayout l;
    l.terminal_width = std::max(40, terminal_width);
    // Fixed columns: arrow + active + source + id + 2 separators.
    int fixed = l.arrow_width + l.active_width + l.source_width +
                l.id_width + 6;
    l.title_width = std::max(20, l.terminal_width - fixed);
    return l;
}

std::string format_row(const Session& s, const RowLayout& layout,
                       bool selected) {
    std::ostringstream o;
    o << (selected ? " → " : "   ");
    std::string name = s.title.empty() ? s.preview : s.title;
    if (name.empty()) name = s.id;
    o << pad(name, static_cast<std::size_t>(layout.title_width));
    o << "  "
      << pad(hermes::cli::preparse::format_relative_time(s.last_active), 10);
    o << "  " << pad(s.source.substr(0, 5), 5);
    o << " " << truncate(s.id, static_cast<std::size_t>(layout.id_width));
    return o.str();
}

std::string format_column_header(const RowLayout& layout) {
    std::ostringstream o;
    o << "   "
      << pad("Title / Preview", static_cast<std::size_t>(layout.title_width))
      << "  " << pad("Active", 10)
      << "  " << pad("Src", 5)
      << " " << "ID";
    return o.str();
}

std::string format_list_row(const Session& s) {
    std::ostringstream o;
    std::string name = s.title.empty() ? s.preview : s.title;
    if (name.empty()) name = s.id;
    o << pad(truncate(name, 40), 40) << "  "
      << pad(hermes::cli::preparse::format_relative_time(s.last_active), 10)
      << "  " << pad(s.source, 8)
      << " " << s.id;
    return o.str();
}

// ---------------------------------------------------------------------------
// Text picker
// ---------------------------------------------------------------------------
std::optional<std::string> text_picker(const std::vector<Session>& sessions,
                                       const PickerOptions& opts,
                                       PickerIO io) {
    auto& in  = io.in  ? *io.in  : std::cin;
    auto& out = io.out ? *io.out : std::cout;
    if (sessions.empty()) {
        out << "No sessions found.\n";
        return std::nullopt;
    }
    auto layout = layout_for_width(80);
    std::size_t page = 0;
    while (true) {
        std::size_t start = page * opts.page_size;
        if (start >= sessions.size()) start = 0;
        std::size_t end = std::min(sessions.size(),
                                   start + opts.page_size);
        out << format_column_header(layout) << "\n";
        for (std::size_t i = start; i < end; ++i) {
            if (opts.show_index) {
                out << std::setw(3) << (i + 1) << ". ";
            }
            out << format_row(sessions[i], layout) << "\n";
        }
        out << "\n" << opts.prompt;
        out.flush();
        std::string line;
        if (!std::getline(in, line)) return std::nullopt;
        if (line.empty()) continue;
        if (line == "q" || line == "Q") return std::nullopt;
        if (line == "n" || line == "N") {
            if (end < sessions.size()) ++page;
            continue;
        }
        if (line == "p" || line == "P") {
            if (page > 0) --page;
            continue;
        }
        try {
            std::size_t idx = std::stoul(line);
            if (idx >= 1 && idx <= sessions.size()) {
                return sessions[idx - 1].id;
            }
        } catch (...) {
        }
        out << "Invalid selection.\n";
    }
}

std::optional<std::string> text_picker(const std::vector<Session>& sessions,
                                       const PickerOptions& opts) {
    return text_picker(sessions, opts, PickerIO{});
}

// ---------------------------------------------------------------------------
// High-level entry points
// ---------------------------------------------------------------------------
int run_browse(std::string* selected) {
    auto sessions = load_sessions();
    if (sessions.empty()) {
        std::cout << "No sessions found.\n";
        if (selected) selected->clear();
        return 0;
    }
    auto pick = text_picker(sessions, PickerOptions{});
    if (pick) {
        if (selected) *selected = *pick;
        std::cout << "Selected: " << *pick << "\n";
    } else {
        if (selected) selected->clear();
        std::cout << "Cancelled.\n";
    }
    return 0;
}

int run_list(const ListOptions& opts, std::ostream& out) {
    auto sessions = load_sessions(opts.source_filter, 2000);
    if (!opts.since_expr.empty()) {
        auto cutoff = std::time(nullptr) - parse_duration(opts.since_expr);
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                           [cutoff](const Session& s) {
                               return s.last_active < cutoff;
                           }),
            sessions.end());
    }
    if (opts.limit > 0 && sessions.size() > opts.limit) {
        sessions.resize(opts.limit);
    }
    if (opts.as_json) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            nlohmann::json j;
            j["id"] = s.id;
            j["title"] = s.title;
            j["preview"] = s.preview;
            j["source"] = s.source;
            j["last_active"] = s.last_active;
            j["message_count"] = s.message_count;
            arr.push_back(j);
        }
        out << arr.dump(2) << "\n";
        return 0;
    }
    if (sessions.empty()) {
        out << "No sessions found.\n";
        return 0;
    }
    for (const auto& s : sessions) {
        out << format_list_row(s) << "\n";
    }
    return 0;
}

}  // namespace hermes::cli::session_browser
