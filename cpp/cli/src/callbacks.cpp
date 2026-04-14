// C++17 port of hermes_cli/callbacks.py — stdin-backed interactive
// callbacks.
#include "hermes/cli/callbacks.hpp"

#include "hermes/cli/colors.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <sys/select.h>
#  include <sys/stat.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace hermes::cli::callbacks {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Read a single line from stdin with a timeout; returns the line (with
// newline stripped) or `std::nullopt` if the timeout expires or input
// closes before a newline arrives.
bool read_line_with_timeout(std::string& out, std::chrono::seconds timeout) {
#ifdef _WIN32
    (void)timeout;
    return static_cast<bool>(std::getline(std::cin, out));
#else
    if (timeout.count() <= 0) {
        return static_cast<bool>(std::getline(std::cin, out));
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv;
    tv.tv_sec = timeout.count();
    tv.tv_usec = 0;
    int ready = ::select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv);
    if (ready <= 0) return false;
    return static_cast<bool>(std::getline(std::cin, out));
#endif
}

// Read a line with echo disabled (for secrets).  Falls back to plain
// read if the terminal cannot be switched.
std::string read_secret_line() {
    std::string line;
#ifndef _WIN32
    termios old_term{};
    bool flipped = false;
    if (::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &old_term) == 0) {
        termios new_term = old_term;
        new_term.c_lflag &= ~ECHO;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term) == 0) {
            flipped = true;
        }
    }
    std::getline(std::cin, line);
    if (flipped) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    std::cout << "\n";
#else
    std::getline(std::cin, line);
#endif
    return line;
}

fs::path env_path() {
    return hermes::core::path::get_hermes_home() / ".env";
}

}  // namespace

std::mutex& approval_mutex() {
    static std::mutex m;
    return m;
}

ClarifyResult clarify_callback(
    const std::string& question,
    const std::vector<std::string>& choices,
    std::chrono::seconds timeout) {
    ClarifyResult r;
    std::cout << "\n  " << col::cyan(question) << "\n";
    if (!choices.empty()) {
        for (std::size_t i = 0; i < choices.size(); ++i) {
            std::cout << "    " << (i + 1) << ") " << choices[i] << "\n";
        }
        std::cout << "  Enter choice [1-" << choices.size() << "]: ";
    } else {
        std::cout << "  Your answer: ";
    }
    std::cout.flush();
    std::string line;
    if (!read_line_with_timeout(line, timeout)) {
        r.timed_out = true;
        r.response = "The user did not provide a response within the time "
                     "limit. Use your best judgement and proceed.";
        std::cout << "\n  " << col::dim("(clarify timed out)") << "\n";
        return r;
    }
    line = trim(line);
    if (!choices.empty()) {
        try {
            int idx = std::stoi(line) - 1;
            if (idx >= 0 && idx < (int)choices.size()) {
                r.choice_index = idx;
                r.response = choices[idx];
                return r;
            }
        } catch (...) {
            // Fall through — user may have typed the literal choice text.
            for (std::size_t i = 0; i < choices.size(); ++i) {
                if (choices[i] == line) {
                    r.choice_index = (int)i;
                    r.response = line;
                    return r;
                }
            }
        }
    }
    r.response = line;
    return r;
}

std::string read_env_value(const std::string& key) {
    std::ifstream f(env_path());
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) {
            std::string v = line.substr(eq + 1);
            // Trim quotes.
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                v = v.substr(1, v.size() - 2);
            }
            return v;
        }
    }
    return "";
}

bool save_env_value_secure(const std::string& key, const std::string& value) {
    auto path = env_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ifstream f(path);
    std::vector<std::string> kept;
    bool replaced = false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) {
            kept.push_back(key + "=" + value);
            replaced = true;
        } else {
            kept.push_back(line);
        }
    }
    if (!replaced) {
        kept.push_back(key + "=" + value);
    }
    std::ofstream out(path);
    if (!out) return false;
    for (const auto& l : kept) out << l << "\n";
#ifndef _WIN32
    // Best-effort chmod 600.
    ::chmod(path.c_str(), 0600);
#endif
    return true;
}

SecretResult prompt_for_secret(const std::string& var_name,
                               const std::string& prompt_text) {
    SecretResult r;
    r.stored_as = var_name;
    std::cout << "\n  " << col::yellow(prompt_text)
              << " (hidden; Enter to skip): ";
    std::cout.flush();
    std::string value = read_secret_line();
    if (value.empty()) {
        r.skipped = true;
        r.reason = "cancelled";
        r.message = "Secret setup was skipped.";
        return r;
    }
    if (!save_env_value_secure(var_name, value)) {
        r.success = false;
        r.message = "Failed to write .env file";
        return r;
    }
    r.validated = true;
    r.message = "Secret stored securely. The value was not exposed to the "
                "model.";
    std::cout << "  " << col::green("\xe2\x9c\x93 Stored ")
              << var_name << " in ~/.hermes/.env\n";
    return r;
}

std::string approval_callback(const std::string& command,
                              const std::string& description,
                              std::chrono::seconds timeout) {
    std::lock_guard<std::mutex> lock(approval_mutex());
    std::cout << "\n  " << col::bold(col::yellow(
                             "Tool approval requested:")) << "\n";
    if (!description.empty()) {
        std::cout << "    " << description << "\n";
    }
    std::string shown = command;
    if (shown.size() > 70) shown = shown.substr(0, 67) + "...";
    std::cout << "    $ " << shown << "\n";
    std::vector<std::string> choices = {"once", "session", "always", "deny"};
    if (command.size() > 70) choices.push_back("view");
    std::cout << "  Choose [";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i) std::cout << "/";
        std::cout << choices[i];
    }
    std::cout << "]: ";
    std::cout.flush();
    std::string line;
    if (!read_line_with_timeout(line, timeout)) {
        std::cout << "\n  " << col::dim("(timeout — denying)") << "\n";
        return "deny";
    }
    line = trim(line);
    std::transform(line.begin(), line.end(), line.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& c : choices) {
        if (line == c) return c;
    }
    // Accept first-letter shortcuts.
    for (const auto& c : choices) {
        if (!c.empty() && !line.empty() && c[0] == line[0]) return c;
    }
    return "deny";
}

}  // namespace hermes::cli::callbacks
