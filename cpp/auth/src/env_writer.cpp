#include "hermes/auth/env_writer.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace hermes::auth {

namespace {

namespace fs = std::filesystem;

// Extract the KEY token from a `.env` line.  Returns an empty string
// for comment / blank / malformed lines.
//
// Supports both `KEY=…` and `export KEY=…`; the leading whitespace
// before the key is tolerated.  Unlike the loader we don't care about
// the VALUE portion — the caller just wants to decide "is this the
// line for `key`?".
std::string extract_key(const std::string& raw_line) {
    // Strip \r (CRLF tolerance) for the key-probe only; the full raw
    // line is preserved by the caller for non-matching rewrites.
    std::string line = raw_line;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }

    // Skip leading whitespace.
    std::size_t i = 0;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= line.size() || line[i] == '#') {
        return {};
    }

    // Optional `export ` prefix.
    static constexpr std::string_view kExport = "export ";
    if (line.compare(i, kExport.size(), kExport) == 0) {
        i += kExport.size();
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
    }

    // Key is [A-Za-z_][A-Za-z0-9_]* (permissive dotenv convention).
    const std::size_t key_start = i;
    if (i >= line.size()) {
        return {};
    }
    const char first = line[i];
    if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_')) {
        return {};
    }
    ++i;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) ||
            line[i] == '_')) {
        ++i;
    }
    const std::size_t key_end = i;

    // Must be followed by `=` (optionally after whitespace).
    std::size_t j = i;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
        ++j;
    }
    if (j >= line.size() || line[j] != '=') {
        return {};
    }
    return line.substr(key_start, key_end - key_start);
}

// True when `raw_line` starts with `export ` (possibly after
// whitespace).  Used to preserve that prefix across in-place updates.
bool had_export_prefix(const std::string& raw_line) {
    std::size_t i = 0;
    while (i < raw_line.size() &&
           std::isspace(static_cast<unsigned char>(raw_line[i]))) {
        ++i;
    }
    static constexpr std::string_view kExport = "export ";
    return raw_line.compare(i, kExport.size(), kExport) == 0;
}

// Format a fresh `KEY=VALUE` line.  Values are emitted verbatim —
// callers that need shell-safe quoting must pre-quote.  This matches
// Python `save_env_value` which also writes the value as-is.
std::string format_kv(const std::string& key,
                      const std::string& value,
                      bool with_export) {
    std::string out;
    out.reserve(key.size() + value.size() + 16);
    if (with_export) {
        out.append("export ");
    }
    out.append(key);
    out.push_back('=');
    out.append(value);
    return out;
}

// Read a file into a list of physical lines WITHOUT stripping the
// trailing newline characters.  The final line gets a synthesised
// newline flag so we can faithfully reconstruct whether the original
// ended with `\n`.
struct LineSlice {
    std::string text;   // content without trailing \r\n
    std::string term;   // "\n", "\r\n", or "" for the final no-EOL line
};

std::vector<LineSlice> read_lines(const fs::path& path, bool& ok) {
    std::vector<LineSlice> out;
    ok = true;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // Non-existence is a legitimate no-content outcome.  The
        // caller decides whether that means "create" or "no-op".
        return out;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return out;
    }
    std::string buf((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i < buf.size()) {
        std::size_t j = buf.find('\n', i);
        if (j == std::string::npos) {
            out.push_back({buf.substr(i), {}});
            break;
        }
        std::string term = "\n";
        std::size_t text_end = j;
        if (text_end > i && buf[text_end - 1] == '\r') {
            --text_end;
            term = "\r\n";
        }
        out.push_back({buf.substr(i, text_end - i), std::move(term)});
        i = j + 1;
    }
    return out;
}

}  // namespace

bool rewrite_env_file(const fs::path& path,
                      const std::unordered_map<std::string, std::string>& updates,
                      const std::unordered_set<std::string>& removals) {
    if (updates.empty() && removals.empty()) {
        return true;
    }

    bool read_ok = true;
    auto lines = read_lines(path, read_ok);
    if (!read_ok) {
        return false;
    }

    std::error_code exists_ec;
    const bool file_existed = !lines.empty() ||
                              fs::exists(path, exists_ec);

    // No file + only removals → nothing to do, success.
    if (!file_existed && updates.empty()) {
        return true;
    }

    // Track which update keys have been applied in-place; everything
    // left over is appended at the tail.
    std::unordered_set<std::string> applied_updates;

    std::vector<LineSlice> out_lines;
    out_lines.reserve(lines.size() + updates.size());

    for (auto& line : lines) {
        const std::string key = extract_key(line.text);
        if (key.empty()) {
            // Comment / blank / unparseable — keep verbatim.
            out_lines.push_back(std::move(line));
            continue;
        }
        if (removals.count(key) > 0) {
            // Drop the line.
            continue;
        }
        if (auto it = updates.find(key); it != updates.end()) {
            const bool keep_export = had_export_prefix(line.text);
            LineSlice rewritten;
            rewritten.text = format_kv(key, it->second, keep_export);
            // Preserve the original line terminator when present.
            rewritten.term = line.term.empty() ? std::string{"\n"} : line.term;
            out_lines.push_back(std::move(rewritten));
            applied_updates.insert(key);
            continue;
        }
        out_lines.push_back(std::move(line));
    }

    // Append any updates we didn't locate in the existing file.
    // Preserve insertion order if we can — but iteration order over an
    // unordered_map isn't stable.  For determinism we collect into a
    // sorted vector so tests remain reproducible.
    std::vector<std::string> pending;
    pending.reserve(updates.size());
    for (const auto& [k, _] : updates) {
        if (applied_updates.count(k) == 0) {
            pending.push_back(k);
        }
    }
    std::sort(pending.begin(), pending.end());

    if (!pending.empty()) {
        // Ensure the previous last line carries a newline before we
        // append a fresh entry — otherwise two lines collide.
        if (!out_lines.empty() && out_lines.back().term.empty()) {
            out_lines.back().term = "\n";
        }
        for (const auto& k : pending) {
            auto it = updates.find(k);
            if (it == updates.end()) continue;  // defensive
            LineSlice ls;
            ls.text = format_kv(k, it->second, /*with_export=*/false);
            ls.term = "\n";
            out_lines.push_back(std::move(ls));
        }
    }

    // Serialise.
    std::ostringstream oss;
    for (const auto& line : out_lines) {
        oss << line.text << line.term;
    }
    const std::string payload = oss.str();

    // atomic_write requires the parent directory to exist.  Create it
    // on demand — mirrors Python's `ensure_hermes_home()` in the same
    // code path.
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Directory-creation failures are reported via the write below.

    return hermes::core::atomic_io::atomic_write(path, payload);
}

bool clear_env_value(const std::string& key) {
    const auto path = hermes::core::path::get_hermes_home() / ".env";
    return rewrite_env_file(path, /*updates=*/{}, /*removals=*/{key});
}

bool set_env_value(const std::string& key, const std::string& value) {
    const auto path = hermes::core::path::get_hermes_home() / ".env";
    std::unordered_map<std::string, std::string> updates;
    updates.emplace(key, value);
    return rewrite_env_file(path, updates, /*removals=*/{});
}

}  // namespace hermes::auth
