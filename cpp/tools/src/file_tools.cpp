// Tool registry glue for file_* tools — C++17 port of the tool-facing
// surface area of `tools/file_operations.py`.
//
// Registrations owned by this TU:
//
//   - read_file   : pagination, image metadata+base64, notebook render,
//                   binary detection, encoding + BOM handling, stat info.
//   - write_file  : atomic write with parent-directory creation, deny list
//                   enforcement, notebook round-trip when editing .ipynb.
//   - patch       : unified-diff application with fuzz + whitespace
//                   tolerance, fallback to ed-style; atomic write.
//   - search_files: regex / literal / files-only / count output modes,
//                   .gitignore-lite via hidden-dir skipping, binary skip,
//                   per-request byte + match budgets, symlink cycle guard.
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/core/atomic_io.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::tools {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace hermes::core;

// ---------------------------------------------------------------------------
// Glob helpers
// ---------------------------------------------------------------------------

namespace {

std::regex glob_to_regex(const std::string& glob) {
    std::string r;
    r.reserve(glob.size() * 2);
    for (char c : glob) {
        switch (c) {
            case '*': r += ".*"; break;
            case '?': r += "."; break;
            case '.': r += "\\."; break;
            case '+': r += "\\+"; break;
            case '(': r += "\\("; break;
            case ')': r += "\\)"; break;
            case '[': r += "\\["; break;
            case ']': r += "\\]"; break;
            case '{': r += "\\{"; break;
            case '}': r += "\\}"; break;
            case '\\': r += "\\\\"; break;
            default: r += c; break;
        }
    }
    return std::regex(r);
}

bool glob_matches(const std::string& filename, const std::string& glob) {
    if (glob.empty()) return true;
    try {
        return std::regex_match(filename, glob_to_regex(glob));
    } catch (...) {
        return false;
    }
}

// Resolve a path relative to the tool-context cwd, with ~user expansion.
fs::path resolve_path(const std::string& raw, const std::string& cwd) {
    std::string expanded = expand_user(raw);
    fs::path p(expanded);
    if (!cwd.empty() && p.is_relative()) p = fs::path(cwd) / p;
    return p;
}

// Detect whether a path component is hidden (starts with '.'), excluding
// "." and "..". Used to skip .git/ etc. during recursive search.
bool is_hidden_component(const fs::path& p) {
    auto s = p.filename().string();
    return s.size() > 1 && s[0] == '.';
}

// Count total lines in a string without materialising line vectors.
int count_lines(std::string_view s) {
    if (s.empty()) return 0;
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// read_file — pagination + binary/image/notebook handling
// ---------------------------------------------------------------------------

namespace {

json build_read_response(const fs::path& fpath,
                         std::int64_t offset_lines,
                         std::int64_t limit_lines,
                         bool raw_mode) {
    json resp;

    auto stat = stat_file(fpath);
    resp["path"] = fpath.string();
    resp["size"] = stat.size;
    if (!stat.mode.empty()) resp["mode"] = stat.mode;
    if (!stat.mtime_iso.empty()) resp["mtime"] = stat.mtime_iso;
    if (stat.is_symlink) resp["symlink_target"] = stat.symlink_target;
    resp["mime"] = detect_mime_type(fpath);

    // Image short-circuit.
    if (is_image_file(fpath)) {
        std::string bytes = read_file_all(fpath);
        auto info = parse_image_info(bytes);
        resp["is_image"] = true;
        resp["format"] = info.format;
        if (info.width && info.height) {
            resp["width"] = info.width;
            resp["height"] = info.height;
            resp["dimensions"] =
                std::to_string(info.width) + "x" + std::to_string(info.height);
        }
        resp["base64"] = base64_encode(bytes);
        return resp;
    }

    // Notebook render.
    if (is_notebook_file(fpath)) {
        std::string raw = read_file_all(fpath);
        auto cells = parse_notebook(raw);
        resp["is_notebook"] = true;
        resp["cell_count"] = static_cast<int>(cells.size());
        resp["content"] = render_notebook(cells);
        return resp;
    }

    // Binary rejection (after extension+magic checks).
    if (is_binary_file(fpath)) {
        resp["is_binary"] = true;
        resp["error"] =
            "binary file — cannot display as text. Use an appropriate tool for this file type.";
        return resp;
    }

    std::string content;
    if (raw_mode || (offset_lines <= 0 && limit_lines < 0)) {
        content = read_file_all(fpath);
    } else {
        // Line-based slicing: read whole file, slice to window.
        content = read_file_all(fpath);
    }

    // Encoding detection + BOM strip.
    auto head = std::string_view(content).substr(
        0, std::min<std::size_t>(content.size(), 4));
    resp["encoding"] = detect_encoding(head);
    strip_bom(content);
    content = normalise_newlines(content);

    if (raw_mode) {
        resp["content"] = content;
        resp["total_lines"] = count_lines(content);
        return resp;
    }

    int total_lines = count_lines(content);
    int start = static_cast<int>(std::max<std::int64_t>(offset_lines, 1));
    int lim = static_cast<int>(
        std::min<std::int64_t>(limit_lines < 0 ? kMaxReadLines : limit_lines,
                               kMaxReadLines));
    int end = start + lim - 1;

    // Slice content by lines.
    std::string slice;
    int ln = 0;
    std::size_t i = 0;
    while (i < content.size() && ln < end) {
        std::size_t j = content.find('\n', i);
        if (j == std::string::npos) j = content.size();
        ++ln;
        if (ln >= start) {
            slice.append(content, i, j - i);
            if (j < content.size()) slice.push_back('\n');
        }
        i = (j == content.size()) ? j : j + 1;
    }

    resp["total_lines"] = total_lines;
    resp["content"] = add_line_numbers(slice, start);
    if (total_lines > end) {
        resp["truncated"] = true;
        resp["hint"] = "Use offset=" + std::to_string(end + 1) +
                       " to continue reading (showing " +
                       std::to_string(start) + "-" + std::to_string(end) +
                       " of " + std::to_string(total_lines) + " lines).";
    }
    return resp;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_file_tools() {
    auto& reg = ToolRegistry::instance();

    // -- read_file ---------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"path",
               {{"type", "string"},
                {"description", "File path (supports ~ expansion)"}}},
              {"offset",
               {{"type", "integer"},
                {"description",
                 "Byte offset when `raw` is true, otherwise a 1-indexed "
                 "line offset"}}},
              {"limit",
               {{"type", "integer"},
                {"description",
                 "Max bytes (raw) or lines (default). -1 reads the remainder."}}},
              {"raw",
               {{"type", "boolean"},
                {"description",
                 "If true, read full content without line numbering / "
                 "pagination"}}}}},
            {"required", json::array({"path"})}};

        ToolEntry entry;
        entry.name = "read_file";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description =
            "Read file contents with pagination, binary / image / notebook "
            "detection, and encoding metadata";
        entry.emoji = "\xF0\x9F\x93\x84";
        entry.max_result_size_chars = 0;
        entry.handler = [](const json& args,
                           const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();
            fs::path fpath = resolve_path(path, ctx.cwd);

            // Block /dev/ paths.
            std::string normalised;
            std::error_code ec;
            if (fs::exists(fpath, ec)) {
                auto c = fs::canonical(fpath, ec);
                normalised = ec ? fs::absolute(fpath).lexically_normal().string()
                                 : c.string();
            } else {
                normalised = fs::absolute(fpath).lexically_normal().string();
            }
            if (normalised.rfind("/dev/", 0) == 0)
                return tool_error("access denied: /dev/ paths are blocked");
            if (!fs::exists(fpath, ec))
                return tool_error("file not found: " + fpath.string());

            // Symlink cycle guard — use the resolved path for reads.
            if (fs::is_symlink(fpath, ec)) {
                auto resolved = resolve_symlink_safe(fpath);
                if (resolved.empty())
                    return tool_error("symlink cycle detected at " +
                                      fpath.string());
                fpath = resolved;
            }

            bool raw = args.value("raw", false);
            std::int64_t offset =
                args.value("offset", std::int64_t{raw ? 0 : 1});
            std::int64_t limit = args.value(
                "limit", std::int64_t{raw ? -1 : 500});

            try {
                if (raw) {
                    // Raw-mode preserves previous byte-offset semantics for
                    // backward compat with existing callers / tests.
                    auto content = read_file_content(
                        fpath,
                        offset > 0 ? offset : 0,
                        limit);
                    json resp;
                    resp["content"] = content;
                    resp["size"] = stat_file(fpath).size;
                    resp["mime"] = detect_mime_type(fpath);
                    return tool_result(resp);
                }
                auto resp = build_read_response(fpath, offset, limit, false);
                return tool_result(resp);
            } catch (const std::exception& e) {
                return tool_error(e.what());
            }
        };
        reg.register_tool(std::move(entry));
    }

    // -- write_file --------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"path",
               {{"type", "string"},
                {"description", "File path to write (supports ~ expansion)"}}},
              {"content",
               {{"type", "string"}, {"description", "Content to write"}}},
              {"create_parents",
               {{"type", "boolean"},
                {"description",
                 "Create parent directories if missing (default true)"}}}}},
            {"required", json::array({"path", "content"})}};

        ToolEntry entry;
        entry.name = "write_file";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description =
            "Write content to a file atomically — creates parents, honours "
            "the write-deny list, preserves notebook metadata on .ipynb.";
        entry.emoji = "\xE2\x9C\x8F\xEF\xB8\x8F";
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args,
                           const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();
            std::string content = args.at("content").get<std::string>();
            bool create_parents = args.value("create_parents", true);

            fs::path fpath = resolve_path(path, ctx.cwd);

            if (is_write_denied(fpath)) {
                return tool_error(
                    "access denied: '" + fpath.string() +
                    "' is a protected system/credential file or outside the "
                    "sandbox.");
            }

            if (create_parents) {
                auto parent = fpath.parent_path();
                if (!parent.empty() && !fs::exists(parent)) {
                    std::error_code ec;
                    fs::create_directories(parent, ec);
                    if (ec)
                        return tool_error("cannot create directories: " +
                                          ec.message());
                }
            }

            // Notebook round-trip: if target is .ipynb and existing content
            // parses as notebook JSON, merge structure to preserve metadata.
            if (is_notebook_file(fpath) && fs::exists(fpath)) {
                try {
                    auto original = read_file_all(fpath);
                    // If incoming content parses as notebook JSON, write as-is.
                    auto probe = json::parse(content, nullptr, false);
                    if (!probe.is_object() || !probe.contains("cells")) {
                        // Treat content as rendered text → wrap as single md cell.
                        std::vector<NotebookCell> cells;
                        NotebookCell nc;
                        nc.cell_type = "markdown";
                        nc.source = content;
                        cells.push_back(std::move(nc));
                        content = edit_notebook(original, cells);
                    }
                } catch (...) {
                    // Fall through: write the raw content.
                }
            }

            if (!atomic_io::atomic_write(fpath, content)) {
                return tool_error("atomic write failed for: " + fpath.string());
            }

            return tool_result(
                {{"written", true},
                 {"path", fpath.string()},
                 {"bytes", static_cast<std::int64_t>(content.size())}});
        };
        reg.register_tool(std::move(entry));
    }

    // -- patch -------------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"path",
               {{"type", "string"}, {"description", "File path to patch"}}},
              {"diff",
               {{"type", "string"},
                {"description",
                 "Unified-diff or ed-style patch to apply"}}},
              {"fuzz",
               {{"type", "integer"},
                {"description",
                 "Line drift tolerated per hunk (default 3)"}}},
              {"ignore_whitespace",
               {{"type", "boolean"},
                {"description",
                 "Ignore leading/trailing whitespace when matching context "
                 "(default true)"}}}}},
            {"required", json::array({"path", "diff"})}};

        ToolEntry entry;
        entry.name = "patch";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description =
            "Apply a unified diff patch to a file — fuzzy context matching, "
            "whitespace-insensitive mode, ed-style fallback.";
        entry.emoji = "\xF0\x9F\xA9\xB9";
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args,
                           const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();
            std::string diff = args.at("diff").get<std::string>();
            int fuzz = args.value("fuzz", 3);
            bool ignore_ws = args.value("ignore_whitespace", true);

            fs::path fpath = resolve_path(path, ctx.cwd);

            if (is_write_denied(fpath))
                return tool_error("access denied: " + fpath.string());

            std::string original;
            try {
                original = read_file_all(fpath);
            } catch (const std::exception& e) {
                return tool_error(std::string("cannot read file: ") + e.what());
            }

            ApplyOptions opts;
            opts.fuzz = fuzz;
            opts.ignore_whitespace = ignore_ws;

            int applied = 0;
            std::string err;
            std::string result =
                apply_unified_diff(original, diff, applied, err, opts);

            if (!err.empty()) {
                // Try ed-style fallback.
                int alt_applied = 0;
                std::string alt_err;
                std::string alt =
                    apply_context_or_ed_diff(original, diff, alt_applied,
                                             alt_err);
                if (alt_err.empty() && alt_applied > 0) {
                    result = alt;
                    applied = alt_applied;
                    err.clear();
                }
            }

            if (!err.empty()) return tool_error(err);

            if (!atomic_io::atomic_write(fpath, result))
                return tool_error("atomic write failed after patch");

            return tool_result(
                {{"patched", true},
                 {"hunks_applied", applied},
                 {"bytes", static_cast<std::int64_t>(result.size())}});
        };
        reg.register_tool(std::move(entry));
    }

    // -- search_files ------------------------------------------------------
    {
        json schema = {
            {"type", "object"},
            {"properties",
             {{"pattern",
               {{"type", "string"},
                {"description",
                 "Regex pattern (or literal with `literal: true`)"}}},
              {"path",
               {{"type", "string"},
                {"description",
                 "Directory to search (default current dir)"}}},
              {"glob",
               {{"type", "string"},
                {"description", "File glob pattern e.g. *.cpp"}}},
              {"literal",
               {{"type", "boolean"},
                {"description",
                 "Treat pattern as a literal string (default false)"}}},
              {"mode",
               {{"type", "string"},
                {"description",
                 "Output mode: 'content' (default), 'files_only', or "
                 "'count'."}}},
              {"max_results",
               {{"type", "integer"},
                {"description", "Maximum matches to return (default 50)"}}},
              {"max_bytes",
               {{"type", "integer"},
                {"description",
                 "Total byte budget scanned across files (default 4 MiB)"}}},
              {"max_files",
               {{"type", "integer"},
                {"description",
                 "Maximum files opened during scan (default 1000)"}}}}},
            {"required", json::array({"pattern"})}};

        ToolEntry entry;
        entry.name = "search_files";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description =
            "Search file contents with regex/literal, with byte/match/file "
            "budgets and binary-file skipping.";
        entry.emoji = "\xF0\x9F\x94\x8D";
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args,
                           const ToolContext& ctx) -> std::string {
            std::string pattern_str = args.at("pattern").get<std::string>();
            std::string search_path = args.value("path", std::string("."));
            std::string glob_pattern = args.value("glob", std::string(""));
            bool literal = args.value("literal", false);
            std::string mode = args.value("mode", std::string("content"));

            SearchBudget budget;
            budget.max_matches = args.value("max_results", budget.max_matches);
            budget.max_bytes = args.value("max_bytes", budget.max_bytes);
            budget.max_files = args.value("max_files", budget.max_files);

            fs::path dir = resolve_path(search_path, ctx.cwd);
            if (!fs::exists(dir) || !fs::is_directory(dir))
                return tool_error("not a directory: " + dir.string());

            // Prepare regex.
            std::regex re;
            try {
                if (literal) {
                    std::string escaped;
                    for (char c : pattern_str) {
                        static const char* meta = R"(.^$|()*+?[]{}\)";
                        if (std::strchr(meta, c)) escaped += '\\';
                        escaped += c;
                    }
                    re = std::regex(escaped);
                } else {
                    re = std::regex(pattern_str);
                }
            } catch (const std::regex_error& e) {
                return tool_error(std::string("invalid regex: ") + e.what());
            }

            json matches = json::array();
            json files_only = json::array();
            std::unordered_map<std::string, int> per_file_count;

            std::int64_t bytes_scanned = 0;
            int files_opened = 0;

            // Walk with symlink cycle guard.
            std::set<std::string> visited_dirs;
            std::error_code walk_ec;
            for (auto it = fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied,
                     walk_ec);
                 it != fs::recursive_directory_iterator();
                 it.increment(walk_ec)) {
                if (walk_ec) continue;
                const auto& p = it->path();

                // Skip hidden dirs (matches Python default).
                if (it->is_directory() && is_hidden_component(p)) {
                    it.disable_recursion_pending();
                    continue;
                }

                // Skip symlink-looped dirs.
                if (it->is_symlink()) {
                    std::error_code sec;
                    auto canon = fs::canonical(p, sec);
                    if (!sec) {
                        if (!visited_dirs.insert(canon.string()).second) {
                            it.disable_recursion_pending();
                            continue;
                        }
                    }
                }

                if (!it->is_regular_file()) continue;
                if (is_binary_file(p)) continue;
                if (!glob_matches(p.filename().string(), glob_pattern)) continue;

                if (++files_opened > budget.max_files) break;

                std::ifstream in(p, std::ios::binary);
                if (!in) continue;

                std::string line;
                int line_no = 0;
                int file_matches = 0;
                bool file_hit = false;
                while (std::getline(in, line)) {
                    ++line_no;
                    bytes_scanned += line.size() + 1;
                    if (bytes_scanned > budget.max_bytes) break;
                    if (std::regex_search(line, re)) {
                        file_hit = true;
                        ++file_matches;
                        if (mode == "content") {
                            if (line.size() > 500)
                                line.resize(500);
                            matches.push_back(
                                {{"file", p.string()},
                                 {"line", line_no},
                                 {"text", line}});
                            if (static_cast<int>(matches.size()) >=
                                budget.max_matches)
                                goto done;
                        }
                    }
                }
                if (file_hit) {
                    if (mode == "files_only")
                        files_only.push_back(p.string());
                    if (mode == "count")
                        per_file_count[p.string()] = file_matches;
                }
                if (bytes_scanned > budget.max_bytes) break;
            }
        done:

            json result;
            if (mode == "files_only") {
                result["files"] = files_only;
                result["total_count"] = static_cast<int>(files_only.size());
            } else if (mode == "count") {
                json counts = json::object();
                int total = 0;
                for (const auto& [k, v] : per_file_count) {
                    counts[k] = v;
                    total += v;
                }
                result["counts"] = counts;
                result["total_count"] = total;
            } else {
                result["matches"] = matches;
                result["total_count"] = static_cast<int>(matches.size());
            }
            result["bytes_scanned"] = bytes_scanned;
            result["files_scanned"] = files_opened;
            return tool_result(result);
        };
        reg.register_tool(std::move(entry));
    }
}

}  // namespace hermes::tools
