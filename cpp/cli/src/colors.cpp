#include "hermes/cli/colors.hpp"

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#endif

#include <cstdlib>
#include <sstream>
#include <string>

namespace hermes::cli::colors {

namespace {

// Cache the TTY check — stdout file descriptor does not change.
bool check_tty() {
    // Respect NO_COLOR convention (https://no-color.org/).
    if (std::getenv("NO_COLOR")) return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

std::string wrap(const std::string& s, const char* code) {
    static const bool tty = check_tty();
    if (!tty) return s;
    return std::string(code) + s + "\033[0m";
}

}  // namespace

bool is_tty() {
    static const bool tty = check_tty();
    return tty;
}

std::string bold(const std::string& s)   { return wrap(s, "\033[1m"); }
std::string dim(const std::string& s)    { return wrap(s, "\033[2m"); }
std::string red(const std::string& s)    { return wrap(s, "\033[31m"); }
std::string green(const std::string& s)  { return wrap(s, "\033[32m"); }
std::string yellow(const std::string& s) { return wrap(s, "\033[33m"); }
std::string blue(const std::string& s)   { return wrap(s, "\033[34m"); }
std::string cyan(const std::string& s)   { return wrap(s, "\033[36m"); }

std::string hex(const std::string& s, const std::string& hex_color) {
    static const bool tty = check_tty();
    if (!tty || hex_color.size() != 6) return s;

    // Parse RRGGBB.
    unsigned int r = 0, g = 0, b = 0;
    try {
        r = static_cast<unsigned int>(std::stoul(hex_color.substr(0, 2), nullptr, 16));
        g = static_cast<unsigned int>(std::stoul(hex_color.substr(2, 2), nullptr, 16));
        b = static_cast<unsigned int>(std::stoul(hex_color.substr(4, 2), nullptr, 16));
    } catch (...) {
        return s;
    }

    std::ostringstream oss;
    oss << "\033[38;2;" << r << ";" << g << ";" << b << "m"
        << s << "\033[0m";
    return oss.str();
}

}  // namespace hermes::cli::colors
