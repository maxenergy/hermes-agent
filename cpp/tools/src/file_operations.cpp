// File operation helpers — C++17 port of `tools/file_operations.py`.
//
// This TU owns the path-safety deny list, binary / image / notebook
// detection, encoding sniffing, image metadata parsing, atomic content
// reading, fuzzy unified-diff application, and Jupyter notebook render /
// edit helpers. The tool-registry glue lives in `file_tools.cpp`.
#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/binary_extensions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hermes::tools {

using json = nlohmann::json;

namespace {

// --------------------------------------------------------------------
// Utility: small helpers
// --------------------------------------------------------------------

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h);
#ifndef _WIN32
    if (auto* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
#endif
    return {};
}

std::string real_path_or_norm(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) {
        auto r = fs::canonical(p, ec);
        if (!ec) return r.string();
    }
    return fs::absolute(p).lexically_normal().string();
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

// --------------------------------------------------------------------
// Deny lists — mirrors Python WRITE_DENIED_PATHS / PREFIXES
// --------------------------------------------------------------------

const std::vector<std::string>& denied_path_suffixes() {
    static const std::vector<std::string> v = {
        "/etc/passwd", "/etc/shadow", "/etc/sudoers",
        "/.ssh/authorized_keys", "/.ssh/id_rsa", "/.ssh/id_ed25519",
        "/.ssh/id_ecdsa", "/.ssh/id_dsa", "/.ssh/config",
        "/.bashrc", "/.zshrc", "/.profile", "/.bash_profile",
        "/.zprofile", "/.netrc", "/.pgpass", "/.npmrc", "/.pypirc",
    };
    return v;
}

const std::vector<std::string>& denied_prefix_suffixes() {
    static const std::vector<std::string> v = {
        "/.ssh/", "/.aws/", "/.gnupg/", "/.kube/",
        "/etc/sudoers.d/", "/etc/systemd/",
        "/.docker/", "/.azure/", "/.config/gh/",
    };
    return v;
}

std::string safe_write_root() {
    const char* r = std::getenv("HERMES_WRITE_SAFE_ROOT");
    if (!r || !*r) return {};
    fs::path p(r);
    if (p.string().size() >= 2 && p.string()[0] == '~') {
        p = fs::path(home_dir()) / p.string().substr(2);
    }
    return real_path_or_norm(p);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API: path safety
// ---------------------------------------------------------------------------

bool is_sensitive_path(const fs::path& p) {
    std::string c = real_path_or_norm(p);
    for (const auto& s : denied_path_suffixes()) {
        if (c.size() >= s.size() &&
            c.compare(c.size() - s.size(), s.size(), s) == 0) {
            return true;
        }
    }
    return false;
}

bool is_write_denied(const fs::path& p) {
    if (is_sensitive_path(p)) return true;

    std::string c = real_path_or_norm(p);

    // Prefix matches: "/home/alice/.ssh/foo" starts with "/home/alice/.ssh/"
    // We test for "<any>/prefix_suffix" — search for the suffix embedded in
    // the canonical path.
    for (const auto& s : denied_prefix_suffixes()) {
        if (c.find(s) != std::string::npos) {
            // Confirm this prefix-suffix lives at a component boundary.
            auto pos = c.find(s);
            if (pos == 0 || c[pos - 1] == '/' || c[pos] == '/') {
                return true;
            }
        }
    }

    // Safe-root sandbox.
    if (auto root = safe_write_root(); !root.empty()) {
        if (c != root && !starts_with(c, root + "/")) return true;
    }
    return false;
}

std::string expand_user(std::string_view path) {
    if (path.empty() || path.front() != '~') return std::string(path);
    auto slash = path.find('/');
    std::string user = (slash == std::string_view::npos)
                           ? std::string(path.substr(1))
                           : std::string(path.substr(1, slash - 1));
    std::string rest =
        (slash == std::string_view::npos) ? "" : std::string(path.substr(slash));
    if (user.empty()) {
        auto h = home_dir();
        return h.empty() ? std::string(path) : h + rest;
    }
#ifndef _WIN32
    // Validate username to avoid exotic characters.
    for (char c : user) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
              c == '_' || c == '-')) {
            return std::string(path);
        }
    }
    if (auto* pw = getpwnam(user.c_str()); pw && pw->pw_dir) {
        return std::string(pw->pw_dir) + rest;
    }
#endif
    return std::string(path);
}

// ---------------------------------------------------------------------------
// Binary / image / notebook detection
// ---------------------------------------------------------------------------

bool is_binary_file(const fs::path& p) {
    if (is_binary_extension(p)) return true;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    char buf[8192];
    in.read(buf, sizeof(buf));
    auto n = in.gcount();
    return is_likely_binary_content(
        std::string_view(buf, static_cast<std::size_t>(n)));
}

bool is_image_file(const fs::path& p) {
    static const std::array<const char*, 7> exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico",
    };
    std::string ext = to_lower(p.extension().string());
    for (auto* e : exts) if (ext == e) return true;
    return false;
}

bool is_notebook_file(const fs::path& p) {
    return to_lower(p.extension().string()) == ".ipynb";
}

// ---------------------------------------------------------------------------
// MIME
// ---------------------------------------------------------------------------

std::string detect_mime_type(const fs::path& p) {
    static const std::unordered_map<std::string, std::string> table = {
        {".txt", "text/plain"},        {".md", "text/markdown"},
        {".html", "text/html"},        {".htm", "text/html"},
        {".css", "text/css"},          {".json", "application/json"},
        {".yaml", "application/yaml"}, {".yml", "application/yaml"},
        {".xml", "application/xml"},   {".js", "application/javascript"},
        {".ts", "application/typescript"},
        {".py", "text/x-python"},      {".cpp", "text/x-c++src"},
        {".cc", "text/x-c++src"},      {".cxx", "text/x-c++src"},
        {".hpp", "text/x-c++hdr"},     {".h", "text/x-chdr"},
        {".c", "text/x-csrc"},         {".go", "text/x-go"},
        {".rs", "text/x-rust"},        {".java", "text/x-java"},
        {".sh", "application/x-sh"},   {".pdf", "application/pdf"},
        {".png", "image/png"},         {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},       {".gif", "image/gif"},
        {".webp", "image/webp"},       {".bmp", "image/bmp"},
        {".ico", "image/x-icon"},      {".svg", "image/svg+xml"},
        {".mp3", "audio/mpeg"},        {".wav", "audio/wav"},
        {".ogg", "audio/ogg"},         {".flac", "audio/flac"},
        {".mp4", "video/mp4"},         {".webm", "video/webm"},
        {".zip", "application/zip"},   {".gz", "application/gzip"},
        {".tar", "application/x-tar"}, {".ipynb", "application/x-ipynb+json"},
    };
    auto it = table.find(to_lower(p.extension().string()));
    if (it != table.end()) return it->second;
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// Encoding / BOM / newlines
// ---------------------------------------------------------------------------

std::string detect_encoding(std::string_view head) {
    auto u = reinterpret_cast<const unsigned char*>(head.data());
    if (head.size() >= 4 && u[0] == 0x00 && u[1] == 0x00 && u[2] == 0xFE &&
        u[3] == 0xFF) return "utf-32be";
    if (head.size() >= 4 && u[0] == 0xFF && u[1] == 0xFE && u[2] == 0x00 &&
        u[3] == 0x00) return "utf-32le";
    if (head.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
        return "utf-8-bom";
    if (head.size() >= 2 && u[0] == 0xFE && u[1] == 0xFF) return "utf-16be";
    if (head.size() >= 2 && u[0] == 0xFF && u[1] == 0xFE) return "utf-16le";
    // Heuristic: valid ASCII / no NUL → utf-8
    for (unsigned char c : head) {
        if (c == 0) return "unknown";
    }
    return "utf-8";
}

std::size_t strip_bom(std::string& content) {
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
        return 3;
    }
    if (content.size() >= 2 &&
        ((static_cast<unsigned char>(content[0]) == 0xFE &&
          static_cast<unsigned char>(content[1]) == 0xFF) ||
         (static_cast<unsigned char>(content[0]) == 0xFF &&
          static_cast<unsigned char>(content[1]) == 0xFE))) {
        content.erase(0, 2);
        return 2;
    }
    return 0;
}

std::string normalise_newlines(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Image metadata
// ---------------------------------------------------------------------------

ImageInfo parse_image_info(std::string_view bytes) {
    ImageInfo info;
    auto u = reinterpret_cast<const unsigned char*>(bytes.data());
    auto n = bytes.size();
    auto be32 = [&](std::size_t o) -> int {
        return (u[o] << 24) | (u[o + 1] << 16) | (u[o + 2] << 8) | u[o + 3];
    };
    auto be16 = [&](std::size_t o) -> int {
        return (u[o] << 8) | u[o + 1];
    };
    auto le16 = [&](std::size_t o) -> int {
        return u[o] | (u[o + 1] << 8);
    };
    auto le32 = [&](std::size_t o) -> int {
        return u[o] | (u[o + 1] << 8) | (u[o + 2] << 16) | (u[o + 3] << 24);
    };

    if (n >= 24 && u[0] == 0x89 && u[1] == 'P' && u[2] == 'N' && u[3] == 'G') {
        info.format = "png";
        info.width = be32(16);
        info.height = be32(20);
        return info;
    }
    if (n >= 6 && u[0] == 'G' && u[1] == 'I' && u[2] == 'F' &&
        u[3] == '8' && (u[4] == '7' || u[4] == '9') && u[5] == 'a') {
        info.format = "gif";
        if (n >= 10) {
            info.width = le16(6);
            info.height = le16(8);
        }
        return info;
    }
    if (n >= 3 && u[0] == 0xFF && u[1] == 0xD8 && u[2] == 0xFF) {
        info.format = "jpeg";
        // Walk JPEG segments to find SOF0/1/2 for dimensions.
        std::size_t i = 2;
        while (i + 8 < n) {
            if (u[i] != 0xFF) break;
            unsigned char marker = u[i + 1];
            if (marker == 0xD8 || marker == 0xD9) { i += 2; continue; }
            int seg_len = be16(i + 2);
            if ((marker >= 0xC0 && marker <= 0xC3) ||
                (marker >= 0xC5 && marker <= 0xC7) ||
                (marker >= 0xC9 && marker <= 0xCB) ||
                (marker >= 0xCD && marker <= 0xCF)) {
                if (i + 9 < n) {
                    info.height = be16(i + 5);
                    info.width = be16(i + 7);
                }
                return info;
            }
            i += 2 + seg_len;
        }
        return info;
    }
    if (n >= 30 && u[0] == 'R' && u[1] == 'I' && u[2] == 'F' && u[3] == 'F' &&
        u[8] == 'W' && u[9] == 'E' && u[10] == 'B' && u[11] == 'P') {
        info.format = "webp";
        if (u[12] == 'V' && u[13] == 'P' && u[14] == '8' && u[15] == ' ') {
            info.width = le16(26) & 0x3FFF;
            info.height = le16(28) & 0x3FFF;
        } else if (u[12] == 'V' && u[13] == 'P' && u[14] == '8' && u[15] == 'L') {
            int b0 = u[21], b1 = u[22], b2 = u[23], b3 = u[24];
            info.width = 1 + (((b1 & 0x3F) << 8) | b0);
            info.height = 1 + (((b3 & 0x0F) << 10) | (b2 << 2) | ((b1 & 0xC0) >> 6));
        } else if (u[12] == 'V' && u[13] == 'P' && u[14] == '8' && u[15] == 'X') {
            // canvas size is 1 + 24-bit LE at offset 24/27
            info.width = 1 + (u[24] | (u[25] << 8) | (u[26] << 16));
            info.height = 1 + (u[27] | (u[28] << 8) | (u[29] << 16));
        }
        return info;
    }
    if (n >= 26 && u[0] == 'B' && u[1] == 'M') {
        info.format = "bmp";
        info.width = le32(18);
        info.height = le32(22);
        return info;
    }
    if (n >= 6 && u[0] == 0 && u[1] == 0 && u[2] == 1 && u[3] == 0) {
        info.format = "ico";
        info.width = u[6] == 0 ? 256 : u[6];
        info.height = u[7] == 0 ? 256 : u[7];
        return info;
    }
    return info;
}

std::string base64_encode(std::string_view bytes) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    auto b = reinterpret_cast<const unsigned char*>(bytes.data());
    for (; i + 3 <= bytes.size(); i += 3) {
        unsigned v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    if (i < bytes.size()) {
        unsigned v = b[i] << 16;
        if (i + 1 < bytes.size()) v |= b[i + 1] << 8;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        if (i + 1 < bytes.size()) {
            out.push_back(kAlphabet[(v >> 6) & 63]);
            out.push_back('=');
        } else {
            out.append("==");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Content reading
// ---------------------------------------------------------------------------

std::string read_file_content(const fs::path& p, int64_t offset, int64_t limit) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + p.string());

    if (offset > 0) {
        in.seekg(offset);
        if (!in) throw std::runtime_error("seek past end of file: " + p.string());
    }

    if (limit < 0) {
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

    std::string buf(static_cast<std::size_t>(limit), '\0');
    in.read(buf.data(), limit);
    buf.resize(static_cast<std::size_t>(in.gcount()));
    return buf;
}

std::string read_file_all(const fs::path& p) {
    return read_file_content(p, 0, -1);
}

FileStat stat_file(const fs::path& p) {
    FileStat out;
    std::error_code ec;
    auto sym = fs::symlink_status(p, ec);
    out.is_symlink = !ec && fs::is_symlink(sym);
    if (out.is_symlink) {
        auto tgt = fs::read_symlink(p, ec);
        if (!ec) out.symlink_target = tgt.string();
    }

#ifndef _WIN32
    struct stat st{};
    if (::stat(p.string().c_str(), &st) == 0) {
        out.size = static_cast<std::int64_t>(st.st_size);
        char mode[10];
        mode[0] = (st.st_mode & S_IRUSR) ? 'r' : '-';
        mode[1] = (st.st_mode & S_IWUSR) ? 'w' : '-';
        mode[2] = (st.st_mode & S_IXUSR) ? 'x' : '-';
        mode[3] = (st.st_mode & S_IRGRP) ? 'r' : '-';
        mode[4] = (st.st_mode & S_IWGRP) ? 'w' : '-';
        mode[5] = (st.st_mode & S_IXGRP) ? 'x' : '-';
        mode[6] = (st.st_mode & S_IROTH) ? 'r' : '-';
        mode[7] = (st.st_mode & S_IWOTH) ? 'w' : '-';
        mode[8] = (st.st_mode & S_IXOTH) ? 'x' : '-';
        mode[9] = '\0';
        out.mode = mode;

        // ISO-8601 UTC.
        std::time_t t = st.st_mtime;
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        out.mtime_iso = buf;
    }
#else
    if (fs::exists(p, ec)) {
        out.size = static_cast<std::int64_t>(fs::file_size(p, ec));
        out.mode = "rw-rw-rw-";
    }
#endif
    return out;
}

fs::path resolve_symlink_safe(const fs::path& p) {
    fs::path cur = p;
    for (int hops = 0; hops < 32; ++hops) {
        std::error_code ec;
        if (!fs::is_symlink(cur, ec)) return cur;
        auto tgt = fs::read_symlink(cur, ec);
        if (ec) return {};
        cur = tgt.is_absolute() ? tgt : cur.parent_path() / tgt;
    }
    return {};  // cycle
}

// ---------------------------------------------------------------------------
// Notebook
// ---------------------------------------------------------------------------

std::vector<NotebookCell> parse_notebook(std::string_view json_text) {
    std::vector<NotebookCell> out;
    try {
        auto j = json::parse(json_text);
        if (!j.is_object() || !j.contains("cells") || !j["cells"].is_array())
            return out;
        for (const auto& cell : j["cells"]) {
            NotebookCell nc;
            if (cell.contains("cell_type") && cell["cell_type"].is_string())
                nc.cell_type = cell["cell_type"].get<std::string>();
            if (cell.contains("source")) {
                if (cell["source"].is_array()) {
                    for (const auto& s : cell["source"])
                        if (s.is_string()) nc.source += s.get<std::string>();
                } else if (cell["source"].is_string()) {
                    nc.source = cell["source"].get<std::string>();
                }
            }
            if (cell.contains("execution_count") &&
                cell["execution_count"].is_number_integer()) {
                nc.execution_count = cell["execution_count"].get<int>();
            }
            if (cell.contains("outputs") && cell["outputs"].is_array()) {
                for (const auto& o : cell["outputs"]) {
                    std::string text;
                    if (o.contains("text")) {
                        if (o["text"].is_array()) {
                            for (const auto& s : o["text"])
                                if (s.is_string())
                                    text += s.get<std::string>();
                        } else if (o["text"].is_string()) {
                            text = o["text"].get<std::string>();
                        }
                    } else if (o.contains("data") && o["data"].is_object()) {
                        if (o["data"].contains("text/plain")) {
                            const auto& tp = o["data"]["text/plain"];
                            if (tp.is_array()) {
                                for (const auto& s : tp)
                                    if (s.is_string())
                                        text += s.get<std::string>();
                            } else if (tp.is_string()) {
                                text = tp.get<std::string>();
                            }
                        }
                    }
                    if (!text.empty()) nc.outputs.push_back(std::move(text));
                }
            }
            out.push_back(std::move(nc));
        }
    } catch (...) {
        return {};
    }
    return out;
}

std::string render_notebook(const std::vector<NotebookCell>& cells) {
    std::ostringstream oss;
    int idx = 0;
    for (const auto& c : cells) {
        ++idx;
        if (c.cell_type == "markdown") {
            oss << "## [markdown cell " << idx << "]\n" << c.source;
        } else if (c.cell_type == "code") {
            oss << "## [code cell " << idx;
            if (c.execution_count >= 0) oss << " in[" << c.execution_count << "]";
            oss << "]\n" << c.source;
            for (const auto& o : c.outputs) {
                oss << "\n--- output ---\n" << o;
            }
        } else {
            oss << "## [" << c.cell_type << " cell " << idx << "]\n" << c.source;
        }
        oss << "\n\n";
    }
    return oss.str();
}

std::string edit_notebook(std::string_view original_json,
                          const std::vector<NotebookCell>& edited_cells) {
    json j;
    try {
        j = json::parse(original_json);
    } catch (...) {
        j = json::object();
    }
    if (!j.is_object()) j = json::object();
    if (!j.contains("nbformat")) j["nbformat"] = 4;
    if (!j.contains("nbformat_minor")) j["nbformat_minor"] = 5;
    if (!j.contains("metadata")) j["metadata"] = json::object();

    json cells_array = json::array();
    for (const auto& c : edited_cells) {
        json cell;
        cell["cell_type"] = c.cell_type.empty() ? "code" : c.cell_type;
        cell["metadata"] = json::object();
        // Split source on '\n' with keepends for notebook round-trip parity.
        json source_lines = json::array();
        std::string::size_type start = 0;
        for (std::string::size_type i = 0; i < c.source.size(); ++i) {
            if (c.source[i] == '\n') {
                source_lines.push_back(c.source.substr(start, i - start + 1));
                start = i + 1;
            }
        }
        if (start < c.source.size())
            source_lines.push_back(c.source.substr(start));
        cell["source"] = source_lines;
        if (cell["cell_type"] == "code") {
            cell["execution_count"] = c.execution_count < 0
                                          ? json(nullptr)
                                          : json(c.execution_count);
            cell["outputs"] = json::array();
            for (const auto& o : c.outputs) {
                cell["outputs"].push_back(
                    {{"output_type", "stream"}, {"name", "stdout"}, {"text", o}});
            }
        }
        cells_array.push_back(std::move(cell));
    }
    j["cells"] = std::move(cells_array);
    return j.dump(1);
}

// ---------------------------------------------------------------------------
// Unified-diff application with fuzz
// ---------------------------------------------------------------------------

namespace {

struct ParsedHunk {
    int old_start = 0;
    int old_len = 0;
    int new_start = 0;
    int new_len = 0;
    std::vector<std::string> lines;  // prefixed with ' '/'+'/'-'
};

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            lines.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size()) lines.emplace_back(s.substr(start));
    return lines;
}

// Parse unified-diff text into hunks. Very tolerant — skips file header
// lines and accepts "@@ -a,b +c,d @@" ranges.
std::vector<ParsedHunk> parse_unified(std::string_view diff) {
    std::vector<ParsedHunk> hunks;
    auto lines = split_lines(diff);
    static const std::regex hunk_re(
        R"(^@@\s+-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s+@@)");
    std::size_t i = 0;
    while (i < lines.size()) {
        std::smatch m;
        if (std::regex_search(lines[i], m, hunk_re)) {
            ParsedHunk h;
            h.old_start = std::stoi(m[1]);
            h.old_len = m[2].matched ? std::stoi(m[2]) : 1;
            h.new_start = std::stoi(m[3]);
            h.new_len = m[4].matched ? std::stoi(m[4]) : 1;
            ++i;
            while (i < lines.size()) {
                const auto& l = lines[i];
                if (l.rfind("@@", 0) == 0) break;
                if (l.rfind("---", 0) == 0 || l.rfind("+++", 0) == 0 ||
                    l.rfind("diff ", 0) == 0 || l.rfind("index ", 0) == 0)
                    break;
                if (!l.empty() && (l[0] == ' ' || l[0] == '+' || l[0] == '-')) {
                    h.lines.push_back(l);
                    ++i;
                    continue;
                }
                if (l.empty()) {
                    h.lines.push_back(" ");  // blank context line
                    ++i;
                    continue;
                }
                break;
            }
            hunks.push_back(std::move(h));
        } else {
            ++i;
        }
    }
    return hunks;
}

bool equal_fuzzy(const std::string& a, const std::string& b,
                 bool ignore_ws) {
    if (a == b) return true;
    if (!ignore_ws) return false;
    auto trim = [](const std::string& s) {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };
    return trim(a) == trim(b);
}

bool match_at(const std::vector<std::string>& file,
              std::size_t pos,
              const std::vector<std::string>& need,
              bool ignore_ws) {
    if (pos + need.size() > file.size()) return false;
    for (std::size_t i = 0; i < need.size(); ++i) {
        if (!equal_fuzzy(file[pos + i], need[i], ignore_ws)) return false;
    }
    return true;
}

}  // namespace

std::string apply_unified_diff(std::string_view original,
                               std::string_view diff,
                               int& applied_out,
                               std::string& err_out,
                               ApplyOptions opts) {
    applied_out = 0;
    err_out.clear();
    auto file = split_lines(original);
    bool trailing_newline = !original.empty() && original.back() == '\n';

    auto hunks = parse_unified(diff);
    if (hunks.empty()) {
        err_out = "no hunks found in diff";
        return std::string(original);
    }

    // Process hunks in reverse so earlier offsets stay valid.
    std::sort(hunks.begin(), hunks.end(),
              [](const ParsedHunk& a, const ParsedHunk& b) {
                  return a.old_start > b.old_start;
              });

    for (const auto& h : hunks) {
        std::vector<std::string> need, out;
        for (const auto& hl : h.lines) {
            char tag = hl.empty() ? ' ' : hl[0];
            std::string body = hl.size() > 1 ? hl.substr(1) : "";
            if (tag == ' ' || tag == '-') need.push_back(body);
            if (tag == ' ' || tag == '+') out.push_back(body);
        }

        int start = h.old_start - 1;
        if (start < 0) start = 0;

        bool matched = false;
        std::size_t matched_pos = 0;

        if (match_at(file, static_cast<std::size_t>(start), need,
                     opts.ignore_whitespace)) {
            matched = true;
            matched_pos = static_cast<std::size_t>(start);
        } else {
            // Search within +/- fuzz lines.
            for (int d = 1; d <= opts.fuzz && !matched; ++d) {
                if (start - d >= 0 &&
                    match_at(file, static_cast<std::size_t>(start - d), need,
                             opts.ignore_whitespace)) {
                    matched_pos = static_cast<std::size_t>(start - d);
                    matched = true;
                    break;
                }
                if (static_cast<std::size_t>(start + d) < file.size() &&
                    match_at(file, static_cast<std::size_t>(start + d), need,
                             opts.ignore_whitespace)) {
                    matched_pos = static_cast<std::size_t>(start + d);
                    matched = true;
                    break;
                }
            }
        }

        if (!matched) {
            err_out = "hunk at line " + std::to_string(h.old_start) +
                      " failed to apply: context mismatch";
            return std::string(original);
        }

        file.erase(file.begin() + matched_pos,
                   file.begin() + matched_pos + need.size());
        file.insert(file.begin() + matched_pos, out.begin(), out.end());
        ++applied_out;
    }

    std::string result;
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (i > 0) result += '\n';
        result += file[i];
    }
    if (trailing_newline) result += '\n';
    return result;
}

std::string apply_context_or_ed_diff(std::string_view original,
                                     std::string_view diff,
                                     int& applied_out,
                                     std::string& err_out) {
    applied_out = 0;
    err_out.clear();
    // Extremely limited ed-style support: accept a sequence of
    // "<lineno>c\n<new content>\n.\n" blocks. Anything else -> error.
    auto lines = split_lines(diff);
    auto file = split_lines(original);
    bool trailing_newline = !original.empty() && original.back() == '\n';
    static const std::regex ed_re(R"(^(\d+)c$)");

    std::size_t i = 0;
    while (i < lines.size()) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, ed_re)) {
            if (lines[i].empty()) { ++i; continue; }
            err_out = "unsupported diff format";
            return std::string(original);
        }
        int ln = std::stoi(m[1]) - 1;
        ++i;
        std::vector<std::string> replacement;
        while (i < lines.size() && lines[i] != ".") {
            replacement.push_back(lines[i]);
            ++i;
        }
        if (i == lines.size()) {
            err_out = "unterminated ed block";
            return std::string(original);
        }
        ++i;  // skip "."
        if (ln < 0 || static_cast<std::size_t>(ln) >= file.size()) {
            err_out = "line out of range";
            return std::string(original);
        }
        file[ln] = replacement.empty() ? "" : replacement.front();
        if (replacement.size() > 1) {
            file.insert(file.begin() + ln + 1, replacement.begin() + 1,
                        replacement.end());
        }
        ++applied_out;
    }

    std::string out;
    for (std::size_t k = 0; k < file.size(); ++k) {
        if (k > 0) out += '\n';
        out += file[k];
    }
    if (trailing_newline) out += '\n';
    return out;
}

// ---------------------------------------------------------------------------
// Line-number rendering
// ---------------------------------------------------------------------------

std::string add_line_numbers(std::string_view content, int start_line) {
    std::string out;
    auto lines = split_lines(content);
    int ln = start_line;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        if (line.size() > static_cast<std::size_t>(kMaxLineLength)) {
            line.resize(kMaxLineLength);
            line += "... [truncated]";
        }
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%6d|", ln);
        out += prefix;
        out += line;
        if (i + 1 < lines.size()) out += '\n';
        ++ln;
    }
    return out;
}

}  // namespace hermes::tools
