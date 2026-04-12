#include "hermes/environments/cwd_tracker.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace hermes::environments {

// ---------------------------------------------------------------------------
// FileCwdTracker
// ---------------------------------------------------------------------------

FileCwdTracker::FileCwdTracker() {
    // Create a temporary file for cwd exchange.
    char tpl[] = "/tmp/hermes-cwd-XXXXXX";
    int fd = ::mkstemp(tpl);
    if (fd >= 0) {
        ::close(fd);
        tmp_file_ = tpl;
    }
}

FileCwdTracker::~FileCwdTracker() {
    if (!tmp_file_.empty()) {
        std::error_code ec;
        fs::remove(tmp_file_, ec);
    }
}

std::string FileCwdTracker::before_run(const std::string& cmd,
                                       const fs::path& /*cwd*/) {
    // Append a cwd-save command after the user's command.
    // Use a subshell group so the exit code of the user's command is
    // preserved via $?.
    std::ostringstream oss;
    oss << cmd << "; __hermes_rc=$?; pwd > "
        << tmp_file_.string()
        << " 2>/dev/null; exit $__hermes_rc";
    return oss.str();
}

fs::path FileCwdTracker::after_run(std::string& /*stdout_text*/) {
    if (tmp_file_.empty()) return {};
    std::ifstream ifs(tmp_file_);
    std::string line;
    if (std::getline(ifs, line) && !line.empty()) {
        return fs::path(line);
    }
    return {};
}

// ---------------------------------------------------------------------------
// MarkerCwdTracker
// ---------------------------------------------------------------------------

std::string MarkerCwdTracker::before_run(const std::string& cmd,
                                         const fs::path& /*cwd*/) {
    std::ostringstream oss;
    oss << cmd
        << "; __hermes_rc=$?; echo '"
        << kMarker << "'\"$(pwd)\"; exit $__hermes_rc";
    return oss.str();
}

fs::path MarkerCwdTracker::after_run(std::string& stdout_text) {
    // Find the last occurrence of the marker in stdout and strip it.
    auto pos = stdout_text.rfind(kMarker);
    if (pos == std::string::npos) return {};

    // Find the end of the line.
    auto eol = stdout_text.find('\n', pos);
    std::string cwd_str;
    if (eol == std::string::npos) {
        cwd_str = stdout_text.substr(pos + std::strlen(kMarker));
        stdout_text.erase(pos);
    } else {
        cwd_str = stdout_text.substr(pos + std::strlen(kMarker),
                                     eol - pos - std::strlen(kMarker));
        stdout_text.erase(pos, eol - pos + 1);
    }

    // Trim trailing whitespace.
    while (!cwd_str.empty() &&
           (cwd_str.back() == '\r' || cwd_str.back() == '\n' ||
            cwd_str.back() == ' ')) {
        cwd_str.pop_back();
    }

    if (cwd_str.empty()) return {};
    return fs::path(cwd_str);
}

}  // namespace hermes::environments
