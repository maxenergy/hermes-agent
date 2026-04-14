// SubdirectoryHintTracker — two complementary trackers:
//
// 1. Legacy LRU: recently-edited directories for startup prompt hints.
// 2. Python-parity tool-arg scanner: discovers AGENTS.md / CLAUDE.md /
//    .cursorrules as the agent navigates into new subdirectories via
//    read_file / terminal / search_files and appends them to tool
//    results. This is the full C++17 port of agent/subdirectory_hints.py.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::agent {

// -- Legacy LRU tracker (unchanged) ----------------------------------

class SubdirectoryHintTracker {
public:
    SubdirectoryHintTracker() = default;
    explicit SubdirectoryHintTracker(size_t capacity) : capacity_(capacity) {}

    void record_edit(const std::filesystem::path& path);
    std::vector<std::string> recent(size_t n = 5) const;
    void clear();
    size_t size() const;

private:
    using Iter = std::list<std::filesystem::path>::iterator;

    mutable std::mutex mu_;
    std::list<std::filesystem::path> order_;
    std::unordered_map<std::string, Iter> index_;
    size_t capacity_ = 64;
};

// -- Python-parity tool-driven discovery -----------------------------

// Tracks which directories the agent has visited and loads hint files
// (AGENTS.md / CLAUDE.md / .cursorrules) on first access. Hints are
// appended to tool results so the model receives fresh context when it
// starts working in a new area of the codebase.
class SubdirectoryHintDiscoverer {
public:
    explicit SubdirectoryHintDiscoverer(std::filesystem::path working_dir = {});

    // Inspect a tool call's arguments, discover any newly-visited
    // directories, and return formatted hint text to append to the tool
    // result (or std::nullopt if there is nothing to append).
    //
    // tool_name: e.g. "read_file", "terminal", "search_files".
    // tool_args: the argument JSON object.
    std::optional<std::string> check_tool_call(
        const std::string& tool_name,
        const nlohmann::json& tool_args);

    // Testing-only hooks.
    const std::unordered_set<std::string>& loaded_dirs_for_testing() const {
        return loaded_dirs_;
    }
    void reset_for_testing();

private:
    std::filesystem::path working_dir_;
    std::unordered_set<std::string> loaded_dirs_;

    std::vector<std::filesystem::path> extract_directories(
        const std::string& tool_name, const nlohmann::json& args);
    void add_path_candidate(
        const std::string& raw_path,
        std::unordered_set<std::string>& out_keys,
        std::vector<std::filesystem::path>& out_list);
    void extract_paths_from_command(
        const std::string& cmd,
        std::unordered_set<std::string>& out_keys,
        std::vector<std::filesystem::path>& out_list);
    bool is_valid_subdir(const std::filesystem::path& p) const;
    std::optional<std::string> load_hints_for_directory(
        const std::filesystem::path& directory);
    std::string relative_display_path(const std::filesystem::path& p) const;
};

// Pure functions exposed for tests.
namespace subdir_hints_detail {

// Lightweight shell tokeniser (whitespace + quotes). No glob/variable
// expansion. Mirrors the shlex.split fallback used in Python.
std::vector<std::string> shell_tokenise(const std::string& cmd);

// Returns true if `token` looks like a path (contains '/' or '.') and is
// not a URL or flag. Used by terminal-command path extraction.
bool looks_like_path_token(const std::string& token);

}  // namespace subdir_hints_detail

}  // namespace hermes::agent
