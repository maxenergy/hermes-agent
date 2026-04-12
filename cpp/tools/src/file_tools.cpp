#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/core/atomic_io.hpp"
#include "hermes/core/patch_parser.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace hermes::tools {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace hermes::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Apply parsed hunks to the file content (line-based).
// Returns the number of successfully applied hunks.  On failure sets `err`.
std::string apply_hunks(const std::string& original,
                        const std::vector<patch_parser::Hunk>& hunks,
                        int& applied, std::string& err) {
    // Split original into lines (preserving empty trailing line).
    std::vector<std::string> lines;
    std::istringstream iss(original);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    applied = 0;
    // Process hunks in reverse order so line numbers stay valid.
    auto sorted = hunks;
    std::sort(sorted.begin(), sorted.end(),
              [](const patch_parser::Hunk& a, const patch_parser::Hunk& b) {
                  return a.old_start > b.old_start;
              });

    for (const auto& hunk : sorted) {
        int start = hunk.old_start - 1;  // 0-based
        if (start < 0) start = 0;

        // Verify context lines match.
        std::vector<std::string> remove_lines;
        std::vector<std::string> add_lines;
        for (const auto& hl : hunk.lines) {
            if (hl.empty()) continue;
            char prefix = hl[0];
            std::string content = hl.size() > 1 ? hl.substr(1) : "";
            if (prefix == ' ' || prefix == '-') {
                remove_lines.push_back(content);
            }
            if (prefix == ' ' || prefix == '+') {
                add_lines.push_back(content);
            }
        }

        // Verify the context.
        bool ok = true;
        if (start + static_cast<int>(remove_lines.size()) >
            static_cast<int>(lines.size())) {
            ok = false;
        } else {
            for (std::size_t i = 0; i < remove_lines.size(); ++i) {
                if (lines[static_cast<std::size_t>(start) + i] !=
                    remove_lines[i]) {
                    ok = false;
                    break;
                }
            }
        }

        if (!ok) {
            err = "hunk " + std::to_string(applied + 1) +
                  " failed to apply: context mismatch";
            return {};
        }

        // Replace.
        auto it = lines.begin() + start;
        lines.erase(it, it + static_cast<long>(remove_lines.size()));
        lines.insert(lines.begin() + start, add_lines.begin(),
                     add_lines.end());
        ++applied;
    }

    // Rejoin.
    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    if (!original.empty() && original.back() == '\n') {
        result += '\n';
    }
    return result;
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
             {{"path", {{"type", "string"}, {"description", "File path to read"}}},
              {"offset",
               {{"type", "integer"},
                {"description", "Byte offset from start (default 0)"}}},
              {"limit",
               {{"type", "integer"},
                {"description",
                 "Maximum bytes to read (-1 = entire file)"}}}}},
            {"required", json::array({"path"})}};

        ToolEntry entry;
        entry.name = "read_file";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description = "Read file contents";
        entry.emoji = "\xF0\x9F\x93\x84";  // page facing up
        entry.max_result_size_chars = 0;     // unlimited
        entry.handler = [](const json& args, const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();

            // Block /dev/ paths.
            fs::path fpath = fs::path(path);
            if (!ctx.cwd.empty() && fpath.is_relative()) {
                fpath = fs::path(ctx.cwd) / fpath;
            }
            std::string normalized;
            try {
                if (fs::exists(fpath))
                    normalized = fs::canonical(fpath).string();
                else
                    normalized = fs::absolute(fpath).lexically_normal().string();
            } catch (...) {
                normalized = fs::absolute(fpath).lexically_normal().string();
            }
            if (normalized.rfind("/dev/", 0) == 0) {
                return tool_error("access denied: /dev/ paths are blocked");
            }

            int64_t offset = args.value("offset", int64_t{0});
            int64_t limit = args.value("limit", int64_t{-1});

            try {
                auto content = read_file_content(fpath, offset, limit);
                return tool_result({{"content", content}});
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
             {{"path", {{"type", "string"}, {"description", "File path to write"}}},
              {"content",
               {{"type", "string"}, {"description", "Content to write"}}}}},
            {"required", json::array({"path", "content"})}};

        ToolEntry entry;
        entry.name = "write_file";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description = "Write content to a file atomically";
        entry.emoji = "\xE2\x9C\x8F\xEF\xB8\x8F";  // pencil
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args, const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();
            std::string content = args.at("content").get<std::string>();

            fs::path fpath(path);
            if (!ctx.cwd.empty() && fpath.is_relative()) {
                fpath = fs::path(ctx.cwd) / fpath;
            }

            if (is_sensitive_path(fpath)) {
                return tool_error("access denied: sensitive path");
            }

            // Create parent directories if needed.
            auto parent = fpath.parent_path();
            if (!parent.empty() && !fs::exists(parent)) {
                std::error_code ec;
                fs::create_directories(parent, ec);
                if (ec) {
                    return tool_error("cannot create directories: " + ec.message());
                }
            }

            if (!atomic_io::atomic_write(fpath, content)) {
                return tool_error("atomic write failed for: " + fpath.string());
            }

            return tool_result({{"written", true},
                                {"path", fpath.string()},
                                {"bytes", static_cast<int64_t>(content.size())}});
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
                {"description", "Unified diff to apply"}}}}},
            {"required", json::array({"path", "diff"})}};

        ToolEntry entry;
        entry.name = "patch";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description = "Apply a unified diff patch to a file";
        entry.emoji = "\xF0\x9F\xA9\xB9";  // adhesive bandage
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args, const ToolContext& ctx) -> std::string {
            std::string path = args.at("path").get<std::string>();
            std::string diff = args.at("diff").get<std::string>();

            fs::path fpath(path);
            if (!ctx.cwd.empty() && fpath.is_relative()) {
                fpath = fs::path(ctx.cwd) / fpath;
            }

            // Read original content.
            std::string original;
            try {
                original = read_file_content(fpath);
            } catch (const std::exception& e) {
                return tool_error(std::string("cannot read file: ") + e.what());
            }

            // Parse the diff.
            auto file_diffs = patch_parser::parse_unified_diff(diff);
            if (file_diffs.empty()) {
                return tool_error("no hunks found in diff");
            }

            // Gather all hunks from all file diffs (typically one).
            std::vector<patch_parser::Hunk> all_hunks;
            for (const auto& fd : file_diffs) {
                all_hunks.insert(all_hunks.end(), fd.hunks.begin(),
                                 fd.hunks.end());
            }

            int applied = 0;
            std::string err;
            std::string result = apply_hunks(original, all_hunks, applied, err);
            if (!err.empty()) {
                return tool_error(err);
            }

            if (!atomic_io::atomic_write(fpath, result)) {
                return tool_error("atomic write failed after patch");
            }

            return tool_result(
                {{"patched", true}, {"hunks_applied", applied}});
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
                {"description", "Regex pattern to search for"}}},
              {"path",
               {{"type", "string"},
                {"description",
                 "Directory to search (default current dir)"}}},
              {"glob",
               {{"type", "string"},
                {"description", "File glob pattern e.g. *.cpp"}}},
              {"max_results",
               {{"type", "integer"},
                {"description", "Maximum matches to return (default 50)"}}}}},
            {"required", json::array({"pattern"})}};

        ToolEntry entry;
        entry.name = "search_files";
        entry.toolset = "file";
        entry.schema = std::move(schema);
        entry.description = "Search file contents with regex";
        entry.emoji = "\xF0\x9F\x94\x8D";  // magnifying glass
        entry.max_result_size_chars = 100 * 1024;
        entry.handler = [](const json& args, const ToolContext& ctx) -> std::string {
            std::string pattern_str = args.at("pattern").get<std::string>();
            std::string search_path = args.value("path", std::string("."));
            std::string glob_pattern = args.value("glob", std::string(""));
            int max_results = args.value("max_results", 50);

            fs::path dir(search_path);
            if (!ctx.cwd.empty() && dir.is_relative()) {
                dir = fs::path(ctx.cwd) / dir;
            }

            if (!fs::exists(dir) || !fs::is_directory(dir)) {
                return tool_error("not a directory: " + dir.string());
            }

            std::regex re;
            try {
                re = std::regex(pattern_str);
            } catch (const std::regex_error& e) {
                return tool_error(std::string("invalid regex: ") + e.what());
            }

            // Simple glob matching (supports * and ?).
            auto glob_matches = [&](const std::string& filename) -> bool {
                if (glob_pattern.empty()) return true;
                // Convert glob to regex.
                std::string re_str;
                for (char c : glob_pattern) {
                    switch (c) {
                        case '*': re_str += ".*"; break;
                        case '?': re_str += "."; break;
                        case '.': re_str += "\\."; break;
                        default: re_str += c; break;
                    }
                }
                try {
                    return std::regex_match(filename, std::regex(re_str));
                } catch (...) {
                    return false;
                }
            };

            json matches = json::array();
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(
                     dir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) continue;
                if (!it->is_regular_file()) continue;
                if (is_binary_file(it->path())) continue;
                if (!glob_matches(it->path().filename().string())) continue;

                // Read and search.
                std::ifstream in(it->path(), std::ios::binary);
                if (!in) continue;

                std::string line;
                int line_no = 0;
                while (std::getline(in, line)) {
                    ++line_no;
                    if (std::regex_search(line, re)) {
                        matches.push_back(
                            {{"file", it->path().string()},
                             {"line", line_no},
                             {"text", line}});
                        if (static_cast<int>(matches.size()) >= max_results) {
                            goto done;
                        }
                    }
                }
            }
        done:
            return tool_result({{"matches", matches}});
        };
        reg.register_tool(std::move(entry));
    }
}

}  // namespace hermes::tools
