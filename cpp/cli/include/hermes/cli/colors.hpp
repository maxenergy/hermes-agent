// ANSI color helpers with TTY detection.
//
// When stdout is not a TTY, all color functions return the input string
// unchanged (no escape sequences emitted).
#pragma once

#include <string>

namespace hermes::cli::colors {

// Returns true if stdout is connected to a terminal.
bool is_tty();

std::string bold(const std::string& s);
std::string dim(const std::string& s);
std::string red(const std::string& s);
std::string green(const std::string& s);
std::string yellow(const std::string& s);
std::string blue(const std::string& s);
std::string cyan(const std::string& s);

// 24-bit true-color: hex_color is "RRGGBB" (no leading '#').
std::string hex(const std::string& s, const std::string& hex_color);

}  // namespace hermes::cli::colors
