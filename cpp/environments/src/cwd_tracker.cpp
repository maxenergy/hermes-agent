#include "hermes/environments/cwd_tracker.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace hermes::environments {

namespace {

// Percent-decode (RFC 3986) the path component of a file:// URL.
// The input is ASCII; we decode %HH hex sequences in place.
std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Locate every OSC 7 sequence in `text`.
// Returns pairs of (start_offset, length_including_terminator).
// Recognised terminators: BEL (0x07) or ESC \ (ST).
std::vector<std::pair<std::size_t, std::size_t>>
find_osc7_sequences(std::string_view text) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    constexpr char kEsc = 0x1B;
    constexpr char kBel = 0x07;

    for (std::size_t i = 0; i + 3 < text.size(); ) {
        // Match ESC ] 7 ;
        if (static_cast<unsigned char>(text[i]) != static_cast<unsigned char>(kEsc) ||
            text[i + 1] != ']' || text[i + 2] != '7' || text[i + 3] != ';') {
            ++i;
            continue;
        }
        std::size_t start = i;
        std::size_t j = i + 4;
        bool terminated = false;
        while (j < text.size()) {
            if (text[j] == kBel) {
                ++j;
                terminated = true;
                break;
            }
            if (text[j] == kEsc && j + 1 < text.size() && text[j + 1] == '\\') {
                j += 2;
                terminated = true;
                break;
            }
            ++j;
        }
        if (terminated) {
            out.emplace_back(start, j - start);
            i = j;
        } else {
            // Unterminated — skip this ESC and keep scanning.
            ++i;
        }
    }
    return out;
}

// Given a full OSC 7 byte-range (starting with ESC ] 7 ; and ending at
// BEL or ESC \), return the decoded local path or empty string if the
// URL doesn't parse.
std::string decode_osc7_payload(std::string_view seq) {
    // Strip leading "ESC ] 7 ;" (4 bytes) and trailing terminator.
    if (seq.size() < 5) return {};
    std::string_view body = seq.substr(4);
    // Drop terminator.
    if (!body.empty() && body.back() == 0x07) {
        body.remove_suffix(1);
    } else if (body.size() >= 2 && body[body.size() - 2] == 0x1B &&
               body.back() == '\\') {
        body.remove_suffix(2);
    }
    // Expect "file://<host>/<path>".  Host may be empty.
    constexpr std::string_view kPrefix = "file://";
    if (body.size() < kPrefix.size() ||
        body.substr(0, kPrefix.size()) != kPrefix) {
        return {};
    }
    body.remove_prefix(kPrefix.size());
    // Skip the host up to the first '/'.
    auto slash = body.find('/');
    if (slash == std::string_view::npos) return {};
    std::string_view path = body.substr(slash);  // includes leading '/'
    return percent_decode(path);
}

}  // namespace

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

// ---------------------------------------------------------------------------
// OscCwdTracker
// ---------------------------------------------------------------------------

fs::path OscCwdTracker::parse_last(std::string_view text) {
    auto seqs = find_osc7_sequences(text);
    if (seqs.empty()) return {};
    const auto& [off, len] = seqs.back();
    auto path = decode_osc7_payload(text.substr(off, len));
    if (path.empty()) return {};
    return fs::path(path);
}

void OscCwdTracker::strip_osc7(std::string& text) {
    auto seqs = find_osc7_sequences(text);
    // Erase from the back so earlier offsets remain valid.
    for (auto it = seqs.rbegin(); it != seqs.rend(); ++it) {
        text.erase(it->first, it->second);
    }
}

fs::path OscCwdTracker::after_run(std::string& stdout_text) {
    auto result = parse_last(stdout_text);
    strip_osc7(stdout_text);
    return result;
}

}  // namespace hermes::environments
