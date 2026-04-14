// C++17 port of hermes_cli/uninstall.py.
#include "hermes/cli/uninstall_cmd.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::cli::uninstall_cmd {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

bool contains_ci(const std::string& s, const std::string& needle) {
    std::string a = to_lower(s);
    std::string b = to_lower(needle);
    return a.find(b) != std::string::npos;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int system_cmd(const std::string& cmd) {
#ifndef _WIN32
    return std::system((cmd + " >/dev/null 2>&1").c_str());
#else
    return std::system(cmd.c_str());
#endif
}

}  // namespace

std::vector<fs::path> find_shell_configs(const fs::path& home) {
    std::vector<fs::path> out;
    for (const char* name : {".bashrc", ".bash_profile", ".profile",
                              ".zshrc", ".zprofile", ".config/fish/config.fish"}) {
        fs::path p = home / name;
        if (fs::exists(p)) out.push_back(p);
    }
    return out;
}

ScrubResult scrub_shell_config(const std::string& original) {
    ScrubResult r;
    std::vector<std::string> lines;
    {
        std::istringstream ss(original);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }
    std::vector<std::string> kept;
    bool skip_next = false;
    for (auto& line : lines) {
        if (line.find("# Hermes Agent") != std::string::npos ||
            line.find("# hermes-agent") != std::string::npos) {
            skip_next = true;
            r.changed = true;
            continue;
        }
        if (skip_next && contains_ci(line, "hermes") &&
            line.find("PATH") != std::string::npos) {
            skip_next = false;
            r.changed = true;
            continue;
        }
        skip_next = false;
        if (contains_ci(line, "hermes") &&
            (line.find("PATH=") != std::string::npos ||
             contains_ci(line, "path="))) {
            r.changed = true;
            continue;
        }
        kept.push_back(line);
    }
    std::ostringstream os;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        os << kept[i];
        if (i + 1 != kept.size()) os << "\n";
    }
    r.content = os.str();
    // Collapse consecutive blank lines.
    std::string compact;
    int blanks = 0;
    for (char c : r.content) {
        if (c == '\n') {
            ++blanks;
            if (blanks <= 2) compact.push_back(c);
        } else {
            blanks = 0;
            compact.push_back(c);
        }
    }
    r.content = compact;
    return r;
}

std::vector<fs::path> find_wrapper_scripts(const fs::path& home) {
    std::vector<fs::path> out;
    for (const fs::path& p : {home / ".local" / "bin" / "hermes",
                               fs::path("/usr/local/bin/hermes")}) {
        if (fs::exists(p)) {
            std::string content = read_file(p);
            if (is_hermes_wrapper(content)) out.push_back(p);
        }
    }
    return out;
}

bool is_hermes_wrapper(const std::string& content) {
    return content.find("hermes_cli") != std::string::npos ||
           content.find("hermes-agent") != std::string::npos ||
           content.find("hermes_cpp") != std::string::npos;
}

namespace {

bool confirm(const std::string& q, bool default_yes) {
    std::cout << "  " << col::yellow(q)
              << (default_yes ? " [Y/n]: " : " [y/N]: ");
    std::string s;
    if (!std::getline(std::cin, s)) return default_yes;
    s = to_lower(trim(s));
    if (s.empty()) return default_yes;
    return s == "y" || s == "yes";
}

int stop_systemd_gateway() {
#ifdef _WIN32
    return 0;
#else
    if (!fs::exists(home_dir() / ".config" / "systemd" / "user" /
                    "hermes-gateway.service")) {
        return 0;
    }
    system_cmd("systemctl --user stop hermes-gateway.service");
    system_cmd("systemctl --user disable hermes-gateway.service");
    std::error_code ec;
    fs::remove(home_dir() / ".config" / "systemd" / "user" /
                   "hermes-gateway.service",
               ec);
    system_cmd("systemctl --user daemon-reload");
    return 1;
#endif
}

}  // namespace

int run(int /*argc*/, char* /*argv*/[]) {
    auto home = home_dir();
    auto hermes_home = hermes::core::path::get_hermes_home();

    std::cout << "\n"
              << col::bold(col::red(
                     "== Hermes Agent Uninstaller ==")) << "\n\n"
              << "  " << col::cyan("Current Installation:") << "\n"
              << "    Home:    " << hermes_home << "\n"
              << "    Config:  " << (hermes_home / "config.yaml") << "\n"
              << "    Secrets: " << (hermes_home / ".env") << "\n\n";

    std::cout << "  " << col::yellow("Uninstall options:") << "\n"
              << "    1) " << col::green("Keep data") << " — remove code "
                 "only, keep configs/sessions/logs\n"
              << "    2) " << col::red("Full uninstall") << " — remove "
                 "everything including data\n"
              << "    3) " << col::cyan("Cancel") << "\n\n";

    std::cout << "  Select [1/2/3]: ";
    std::string choice;
    if (!std::getline(std::cin, choice)) {
        std::cout << "  Cancelled.\n";
        return 0;
    }
    choice = trim(choice);
    if (choice == "3" || choice.empty() ||
        to_lower(choice) == "cancel" || to_lower(choice) == "c" ||
        to_lower(choice) == "q" || to_lower(choice) == "no") {
        std::cout << "  Cancelled.\n";
        return 0;
    }
    bool full = (choice == "2");

    if (full) {
        std::cout << "  " << col::red("WARNING: ")
                  << "This will permanently delete ALL Hermes data!\n";
    } else {
        std::cout << "  This will remove code but keep configs/data.\n";
    }

    if (!confirm("Type 'yes' to confirm", false)) {
        std::cout << "  Cancelled.\n";
        return 0;
    }

    std::cout << "\n  " << col::cyan("Uninstalling...") << "\n";

    // 1. Gateway service.
    if (stop_systemd_gateway()) {
        std::cout << "  " << col::green("\xe2\x9c\x93")
                  << " Removed gateway service\n";
    }
    // 2. Shell configs.
    int touched = 0;
    for (const auto& rc : find_shell_configs(home)) {
        auto before = read_file(rc);
        auto r = scrub_shell_config(before);
        if (r.changed) {
            std::ofstream f(rc);
            if (f) {
                f << r.content;
                ++touched;
                std::cout << "  " << col::green("\xe2\x9c\x93")
                          << " Scrubbed " << rc << "\n";
            }
        }
    }
    if (touched == 0) {
        std::cout << "    (no PATH entries found to scrub)\n";
    }
    // 3. Wrapper scripts.
    for (const auto& w : find_wrapper_scripts(home)) {
        std::error_code ec;
        fs::remove(w, ec);
        if (!ec) {
            std::cout << "  " << col::green("\xe2\x9c\x93")
                      << " Removed wrapper " << w << "\n";
        }
    }
    // 4. Hermes home (full mode only).
    if (full) {
        std::error_code ec;
        auto removed = fs::remove_all(hermes_home, ec);
        if (!ec) {
            std::cout << "  " << col::green("\xe2\x9c\x93")
                      << " Removed " << hermes_home
                      << " (" << removed << " entries)\n";
        } else {
            std::cerr << "  " << col::red("\xe2\x9c\x97")
                      << " Failed to remove " << hermes_home << ": "
                      << ec.message() << "\n";
        }
    } else {
        std::cout << "    Keeping configs/data under " << hermes_home << "\n";
    }

    std::cout << "\n  " << col::green(col::bold("Uninstall complete."))
              << "\n\n";
    return 0;
}

}  // namespace hermes::cli::uninstall_cmd
