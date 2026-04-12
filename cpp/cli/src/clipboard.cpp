#include "hermes/cli/clipboard.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace hermes::cli {

namespace {

bool command_exists(const char* cmd) {
    std::string check = std::string("which ") + cmd + " > /dev/null 2>&1";
    return std::system(check.c_str()) == 0;
}

}  // namespace

bool copy_to_clipboard(const std::string& text) {
#if defined(__APPLE__)
    FILE* pipe = popen("pbcopy", "w");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
#elif defined(__linux__)
    const char* cmd = nullptr;
    if (command_exists("xclip")) {
        cmd = "xclip -selection clipboard";
    } else if (command_exists("xsel")) {
        cmd = "xsel --clipboard --input";
    } else if (command_exists("wl-copy")) {
        cmd = "wl-copy";
    }
    if (!cmd) return false;

    FILE* pipe = popen(cmd, "w");
    if (!pipe) return false;
    std::fwrite(text.data(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
#else
    (void)text;
    return false;
#endif
}

std::string paste_from_clipboard() {
#if defined(__APPLE__)
    const char* cmd = "pbpaste";
#elif defined(__linux__)
    const char* cmd = nullptr;
    if (command_exists("xclip")) {
        cmd = "xclip -selection clipboard -o";
    } else if (command_exists("xsel")) {
        cmd = "xsel --clipboard --output";
    } else if (command_exists("wl-paste")) {
        cmd = "wl-paste";
    }
    if (!cmd) return {};
#else
    return {};
#endif

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};

    std::string result;
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        result.append(buf.data(), n);
    }
    pclose(pipe);
    return result;
}

}  // namespace hermes::cli
