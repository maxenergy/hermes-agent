// ToolContext — base64 upload/download helpers and terminal-result parsing.
// See the header for context; this file is a port of the pure-logic pieces
// of environments/tool_context.py (the tool-dispatch side stays in Python).
#include "hermes/environments/tool_context.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::environments::tool_context {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

namespace {

const std::array<char, 65>& b64_alphabet() {
    static const std::array<char, 65> a = {{
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
        'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f',
        'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v',
        'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/',
        '=',
    }};
    return a;
}

int b64_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(const std::uint8_t* data, std::size_t n) {
    const auto& alpha = b64_alphabet();
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= n) {
        std::uint32_t w = (std::uint32_t(data[i]) << 16)
                        | (std::uint32_t(data[i + 1]) << 8)
                        |  std::uint32_t(data[i + 2]);
        out.push_back(alpha[(w >> 18) & 0x3F]);
        out.push_back(alpha[(w >> 12) & 0x3F]);
        out.push_back(alpha[(w >> 6)  & 0x3F]);
        out.push_back(alpha[ w        & 0x3F]);
        i += 3;
    }
    if (i < n) {
        std::uint32_t w = std::uint32_t(data[i]) << 16;
        if (i + 1 < n) w |= std::uint32_t(data[i + 1]) << 8;
        out.push_back(alpha[(w >> 18) & 0x3F]);
        out.push_back(alpha[(w >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? alpha[(w >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(std::string_view bytes) {
    return base64_encode(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                         bytes.size());
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view encoded) {
    // Strip whitespace (spaces, tabs, newlines, CRs).
    std::string buf;
    buf.reserve(encoded.size());
    for (char c : encoded) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        buf.push_back(c);
    }
    if (buf.size() % 4 != 0) return std::nullopt;

    std::vector<std::uint8_t> out;
    out.reserve(buf.size() / 4 * 3);
    for (std::size_t i = 0; i < buf.size(); i += 4) {
        int a = b64_index(buf[i]);
        int b = b64_index(buf[i + 1]);
        char c_ch = buf[i + 2], d_ch = buf[i + 3];
        if (a < 0 || b < 0) return std::nullopt;
        int c = (c_ch == '=') ? -2 : b64_index(c_ch);
        int d = (d_ch == '=') ? -2 : b64_index(d_ch);
        if (c == -1 || d == -1) return std::nullopt;

        std::uint32_t w = (std::uint32_t(a) << 18) | (std::uint32_t(b) << 12);
        if (c >= 0) w |= std::uint32_t(c) << 6;
        if (d >= 0) w |= std::uint32_t(d);

        out.push_back(static_cast<std::uint8_t>((w >> 16) & 0xFF));
        if (c >= 0) out.push_back(static_cast<std::uint8_t>((w >> 8) & 0xFF));
        if (d >= 0) out.push_back(static_cast<std::uint8_t>( w       & 0xFF));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Shell helpers
// ---------------------------------------------------------------------------

std::string shell_single_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            // Close, escape, reopen.
            out.append("'\\''");
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::optional<std::string> remote_mkdir_parent(std::string_view remote_path) {
    // Python equivalent: `parent = str(_Path(remote_path).parent)` and skip
    // if parent in (".", "/").
    auto slash = remote_path.find_last_of('/');
    if (slash == std::string_view::npos) return std::nullopt;  // parent = "."
    if (slash == 0) return std::nullopt;                       // parent = "/"
    return std::string(remote_path.substr(0, slash));
}

// ---------------------------------------------------------------------------
// Upload plan
// ---------------------------------------------------------------------------

UploadPlan build_upload_plan(const std::uint8_t* data, std::size_t n,
                              std::string_view remote_path,
                              std::size_t chunk_size) {
    UploadPlan plan;
    auto b64 = base64_encode(data, n);
    plan.encoded_size = b64.size();

    if (b64.size() <= chunk_size) {
        // Single-command upload.
        std::string cmd = "printf '%s' '";
        cmd += b64;
        cmd += "' | base64 -d > ";
        cmd += std::string(remote_path);
        plan.commands.push_back(std::move(cmd));
        plan.chunked = false;
        return plan;
    }

    plan.chunked = true;
    const std::string tmp = "/tmp/_hermes_upload.b64";
    // Truncate the staging file.
    plan.commands.push_back(": > " + tmp);
    for (std::size_t i = 0; i < b64.size(); i += chunk_size) {
        std::string chunk = b64.substr(i, chunk_size);
        plan.commands.push_back("printf '%s' '" + chunk + "' >> " + tmp);
    }
    std::string finalize = "base64 -d " + tmp + " > " + std::string(remote_path)
                          + " && rm -f " + tmp;
    plan.commands.push_back(std::move(finalize));
    return plan;
}

UploadPlan build_upload_plan(std::string_view bytes,
                              std::string_view remote_path,
                              std::size_t chunk_size) {
    return build_upload_plan(
        reinterpret_cast<const std::uint8_t*>(bytes.data()),
        bytes.size(),
        remote_path,
        chunk_size);
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

std::string build_download_command(std::string_view remote_path) {
    // The Python tool uses `base64 <remote> 2>/dev/null`.  Reproduce
    // exactly: no output capture flags needed, the terminal tool returns
    // `{"output": <stdout>}`.
    std::string cmd = "base64 ";
    cmd += std::string(remote_path);
    cmd += " 2>/dev/null";
    return cmd;
}

DownloadResult decode_download_output(std::string_view terminal_output) {
    DownloadResult r;
    // Strip leading / trailing whitespace.
    std::size_t start = 0, end = terminal_output.size();
    while (start < end && std::isspace(static_cast<unsigned char>(terminal_output[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(terminal_output[end - 1]))) --end;
    auto trimmed = terminal_output.substr(start, end - start);

    if (trimmed.empty()) {
        r.error = "Remote file is empty or missing";
        return r;
    }
    auto decoded = base64_decode(trimmed);
    if (!decoded) {
        r.error = "Base64 decode failed";
        return r;
    }
    r.success = true;
    r.bytes = std::move(*decoded);
    return r;
}

// ---------------------------------------------------------------------------
// Directory helpers
// ---------------------------------------------------------------------------

std::string build_remote_find_command(std::string_view remote_dir) {
    std::string cmd = "find ";
    cmd += std::string(remote_dir);
    cmd += " -type f 2>/dev/null";
    return cmd;
}

std::vector<std::string> parse_find_output(std::string_view output) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : output) {
        if (c == '\n') {
            // Strip trailing whitespace.
            while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) {
                cur.pop_back();
            }
            // Strip leading whitespace too.
            std::size_t s = 0;
            while (s < cur.size() && std::isspace(static_cast<unsigned char>(cur[s]))) ++s;
            if (s > 0) cur.erase(0, s);
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
    {
        std::size_t s = 0;
        while (s < cur.size() && std::isspace(static_cast<unsigned char>(cur[s]))) ++s;
        if (s > 0) cur.erase(0, s);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string relative_remote_path(std::string_view remote_file,
                                  std::string_view remote_root) {
    if (remote_file.rfind(remote_root, 0) == 0) {
        auto rel = remote_file.substr(remote_root.size());
        // Strip leading slashes (matches Python .lstrip("/")).
        std::size_t s = 0;
        while (s < rel.size() && rel[s] == '/') ++s;
        return std::string(rel.substr(s));
    }
    // Basename fallback.
    auto slash = remote_file.find_last_of('/');
    if (slash == std::string_view::npos) return std::string(remote_file);
    return std::string(remote_file.substr(slash + 1));
}

std::vector<LocalFileEntry> enumerate_local_dir(const fs::path& dir) {
    std::vector<LocalFileEntry> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code lec;
        if (!fs::is_regular_file(it->path(), lec)) continue;
        LocalFileEntry e;
        e.absolute = it->path();
        // Relative path with POSIX-style separators — tokens joined by "/".
        auto rel = fs::relative(it->path(), dir, lec);
        std::string rel_str;
        bool first = true;
        for (const auto& part : rel) {
            if (!first) rel_str.push_back('/');
            rel_str += part.string();
            first = false;
        }
        e.relative = std::move(rel_str);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const LocalFileEntry& a, const LocalFileEntry& b) {
        return a.absolute < b.absolute;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Terminal-result parsing
// ---------------------------------------------------------------------------

ExecResult parse_terminal_result(std::string_view input) {
    ExecResult r;
    auto j = nlohmann::json::parse(input, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        r.exit_code = -1;
        r.output = std::string(input);
        return r;
    }
    if (j.contains("exit_code")) {
        try { r.exit_code = j["exit_code"].get<int>(); }
        catch (...) { r.exit_code = -1; }
    }
    if (j.contains("output") && j["output"].is_string()) {
        r.output = j["output"].get<std::string>();
    }
    return r;
}

}  // namespace hermes::environments::tool_context
