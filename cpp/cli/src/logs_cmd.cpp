// C++17 port of hermes_cli/logs.py — tail/follow/filter for hermes logs.
#include "hermes/cli/logs_cmd.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace hermes::cli::logs_cmd {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

namespace {

const std::map<std::string, std::string>& filename_map() {
    static const std::map<std::string, std::string> m = {
        {"agent", "agent.log"},
        {"errors", "errors.log"},
        {"gateway", "gateway.log"},
    };
    return m;
}

const std::unordered_map<std::string, int>& level_rank() {
    static const std::unordered_map<std::string, int> m = {
        {"DEBUG", 0}, {"INFO", 1}, {"WARNING", 2},
        {"ERROR", 3}, {"CRITICAL", 4},
    };
    return m;
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

}  // namespace

fs::path log_path_for(const std::string& name) {
    auto it = filename_map().find(name);
    std::string fn = it == filename_map().end() ? (name + ".log") : it->second;
    return hermes::core::path::get_hermes_home() / "logs" / fn;
}

std::optional<std::chrono::system_clock::time_point>
parse_since(const std::string& s) {
    static const std::regex re(R"(^\s*(\d+)\s*([smhd])\s*$)",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_match(s, m, re)) return std::nullopt;
    int n = std::stoi(m[1].str());
    char unit = std::tolower(m[2].str()[0]);
    std::chrono::seconds delta(0);
    switch (unit) {
        case 's': delta = std::chrono::seconds(n); break;
        case 'm': delta = std::chrono::minutes(n); break;
        case 'h': delta = std::chrono::hours(n); break;
        case 'd': delta = std::chrono::hours(n * 24); break;
        default: return std::nullopt;
    }
    return std::chrono::system_clock::now() - delta;
}

std::optional<std::string> extract_level(const std::string& line) {
    static const std::regex re(
        R"(\s(DEBUG|INFO|WARNING|ERROR|CRITICAL)\s)");
    std::smatch m;
    if (!std::regex_search(line, m, re)) return std::nullopt;
    return m[1].str();
}

std::optional<std::chrono::system_clock::time_point>
extract_timestamp(const std::string& line) {
    static const std::regex re(
        R"(^(\d{4})-(\d{2})-(\d{2})\s+(\d{2}):(\d{2}):(\d{2}))");
    std::smatch m;
    if (!std::regex_search(line, m, re)) return std::nullopt;
    std::tm tm{};
    tm.tm_year = std::stoi(m[1].str()) - 1900;
    tm.tm_mon  = std::stoi(m[2].str()) - 1;
    tm.tm_mday = std::stoi(m[3].str());
    tm.tm_hour = std::stoi(m[4].str());
    tm.tm_min  = std::stoi(m[5].str());
    tm.tm_sec  = std::stoi(m[6].str());
    std::time_t tt = std::mktime(&tm);
    if (tt == -1) return std::nullopt;
    return std::chrono::system_clock::from_time_t(tt);
}

bool line_passes(const std::string& line, const Options& opts,
                 std::optional<std::chrono::system_clock::time_point>
                     since_cutoff) {
    if (since_cutoff) {
        auto ts = extract_timestamp(line);
        if (ts && *ts < *since_cutoff) return false;
    }
    if (!opts.min_level.empty()) {
        auto lvl = extract_level(line);
        if (lvl) {
            int rank = level_rank().count(*lvl)
                           ? level_rank().at(*lvl) : 0;
            int minr = level_rank().count(opts.min_level)
                           ? level_rank().at(opts.min_level) : 0;
            if (rank < minr) return false;
        }
    }
    if (!opts.session_substr.empty()) {
        if (line.find(opts.session_substr) == std::string::npos) return false;
    }
    return true;
}

std::vector<std::string> read_last_n_lines(const fs::path& path, int n) {
    std::vector<std::string> lines;
    if (!fs::exists(path)) return lines;
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return lines;
    std::ifstream f(path);
    if (!f) return lines;
    if (sz <= 1024 * 1024) {
        std::string line;
        while (std::getline(f, line)) lines.push_back(line);
        if ((int)lines.size() > n) {
            lines.erase(lines.begin(), lines.end() - n);
        }
        return lines;
    }
    // Large file — chunk read from the end.
    f.seekg(0, std::ios::end);
    std::streampos pos = f.tellg();
    const std::streamsize chunk = 8192;
    std::string buf;
    while (pos > 0 && (int)lines.size() <= n + 1) {
        std::streamsize read = std::min<std::streamsize>(chunk, pos);
        pos -= read;
        f.seekg(pos, std::ios::beg);
        std::string tmp(read, '\0');
        f.read(&tmp[0], read);
        buf = tmp + buf;
        lines.clear();
        std::istringstream ss(buf);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }
    if ((int)lines.size() > n) {
        lines.erase(lines.begin(), lines.end() - n);
    }
    return lines;
}

int cmd_list() {
    auto dir = hermes::core::path::get_hermes_home() / "logs";
    if (!fs::exists(dir)) {
        std::cout << "  No logs directory at " << dir << "\n";
        return 0;
    }
    std::cout << "  Log files in " << dir << ":\n\n";
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         ++it) {
        if (it->is_regular_file() && it->path().extension() == ".log") {
            entries.push_back(*it);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename() < b.path().filename();
              });
    for (const auto& e : entries) {
        auto sz = fs::file_size(e.path(), ec);
        std::string size_str;
        if (sz < 1024) size_str = std::to_string(sz) + "B";
        else if (sz < 1024 * 1024) size_str =
            std::to_string(sz / 1024) + "KB";
        else size_str = std::to_string(sz / (1024 * 1024)) + "MB";
        std::cout << "    " << std::left;
        std::cout.width(30);
        std::cout << e.path().filename().string() << size_str << "\n";
    }
    return 0;
}

int cmd_tail(const Options& opts) {
    auto path = log_path_for(opts.log_name);
    if (!fs::exists(path)) {
        std::cerr << "  No log file at " << path << "\n";
        return 1;
    }
    auto since_cut = opts.since_rel.empty() ? std::nullopt
                                            : parse_since(opts.since_rel);
    if (!opts.since_rel.empty() && !since_cut) {
        std::cerr << "  Invalid --since: " << opts.since_rel << "\n";
        return 1;
    }
    bool has_filter = !opts.min_level.empty() ||
                      !opts.session_substr.empty() ||
                      since_cut.has_value();
    auto raw = read_last_n_lines(path,
                                 has_filter ? opts.num_lines * 20
                                            : opts.num_lines);
    std::vector<std::string> shown;
    for (const auto& line : raw) {
        if (!has_filter || line_passes(line, opts, since_cut)) {
            shown.push_back(line);
        }
    }
    if ((int)shown.size() > opts.num_lines) {
        shown.erase(shown.begin(), shown.end() - opts.num_lines);
    }
    for (const auto& line : shown) {
        std::cout << line << "\n";
    }
    if (!opts.follow) return 0;
    // Poll for new lines.
    std::ifstream f(path);
    f.seekg(0, std::ios::end);
    while (true) {
        std::string line;
        while (std::getline(f, line)) {
            if (!has_filter || line_passes(line, opts, since_cut)) {
                std::cout << line << "\n";
                std::cout.flush();
            }
        }
        f.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

int run(int argc, char* argv[]) {
    Options opts;
    bool list_mode = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list" || a == "list") { list_mode = true; }
        else if (a == "--follow" || a == "-f") opts.follow = true;
        else if ((a == "--tail" || a == "-n") && i + 1 < argc) {
            try { opts.num_lines = std::stoi(argv[++i]); } catch (...) {}
        }
        else if (a == "--level" && i + 1 < argc) {
            opts.min_level = to_upper(argv[++i]);
        }
        else if (a == "--session" && i + 1 < argc) {
            opts.session_substr = argv[++i];
        }
        else if (a == "--since" && i + 1 < argc) {
            opts.since_rel = argv[++i];
        }
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: hermes logs [agent|errors|gateway] "
                         "[--tail N] [--follow]\n"
                      << "                     [--level LEVEL] [--session ID] "
                         "[--since REL]\n";
            return 0;
        }
        else if (!a.empty() && a[0] != '-') {
            opts.log_name = a;
        }
    }
    if (list_mode) return cmd_list();
    return cmd_tail(opts);
}

}  // namespace hermes::cli::logs_cmd
