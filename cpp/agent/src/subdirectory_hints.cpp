// Dual tracker: legacy LRU + Python-parity discovery.
#include "hermes/agent/subdirectory_hints.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace hermes::agent {

namespace fs = std::filesystem;

// ── Legacy LRU ─────────────────────────────────────────────────────────

namespace {

fs::path normalise_to_dir(const fs::path& p) {
    if (p.empty()) return p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p;
    auto parent = p.parent_path();
    return parent.empty() ? p : parent;
}

}  // namespace

void SubdirectoryHintTracker::record_edit(const fs::path& path) {
    auto dir = normalise_to_dir(path);
    if (dir.empty()) return;
    const std::string key = dir.string();

    std::lock_guard<std::mutex> lock(mu_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        order_.erase(it->second);
        index_.erase(it);
    }
    order_.push_front(dir);
    index_[key] = order_.begin();

    while (order_.size() > capacity_) {
        const std::string old_key = order_.back().string();
        index_.erase(old_key);
        order_.pop_back();
    }
}

std::vector<std::string> SubdirectoryHintTracker::recent(size_t n) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(std::min(n, order_.size()));
    for (const auto& p : order_) {
        if (out.size() >= n) break;
        out.push_back(p.string());
    }
    return out;
}

void SubdirectoryHintTracker::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    order_.clear();
    index_.clear();
}

size_t SubdirectoryHintTracker::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return order_.size();
}

// ── Python-parity discoverer ───────────────────────────────────────────

namespace {

constexpr const char* k_hint_filenames[] = {
    "AGENTS.md", "agents.md", "CLAUDE.md", "claude.md", ".cursorrules",
};
constexpr std::size_t k_max_hint_chars = 8000;
constexpr int k_max_ancestor_walk = 5;

const std::unordered_set<std::string>& path_arg_keys() {
    static const std::unordered_set<std::string> keys = {"path", "file_path", "workdir"};
    return keys;
}

bool is_command_tool(const std::string& name) { return name == "terminal"; }

// Expand a leading ~ against $HOME when present. No other env expansion.
fs::path expanduser(const std::string& s) {
    if (s.empty() || s[0] != '~') return fs::path(s);
    const char* home = std::getenv("HOME");
    if (home == nullptr) return fs::path(s);
    // Support "~" and "~/rest"
    if (s.size() == 1) return fs::path(home);
    if (s[1] == '/') return fs::path(std::string(home) + s.substr(1));
    return fs::path(s);
}

std::string read_file_to_string(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

std::string strip(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin());
}

// Format char count as "12,345".
std::string format_with_commas(std::size_t n) {
    std::string raw = std::to_string(n);
    std::string out;
    int cnt = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (cnt > 0 && cnt % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++cnt;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

namespace subdir_hints_detail {

std::vector<std::string> shell_tokenise(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    char quote = '\0';
    bool escape = false;
    for (char c : cmd) {
        if (escape) {
            cur += c;
            escape = false;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            escape = true;
            continue;
        }
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool looks_like_path_token(const std::string& token) {
    if (token.empty()) return false;
    if (token[0] == '-') return false;
    if (starts_with(token, "http://") || starts_with(token, "https://") ||
        starts_with(token, "git@")) {
        return false;
    }
    if (token.find('/') == std::string::npos && token.find('.') == std::string::npos) {
        return false;
    }
    return true;
}

}  // namespace subdir_hints_detail

// ── Class impl ──────────────────────────────────────────────────────

SubdirectoryHintDiscoverer::SubdirectoryHintDiscoverer(fs::path working_dir) {
    std::error_code ec;
    working_dir_ = working_dir.empty() ? fs::current_path(ec)
                                       : fs::absolute(working_dir, ec);
    if (ec) working_dir_ = working_dir;
    // Pre-mark working dir as loaded (startup context handles it).
    fs::path canonical = fs::weakly_canonical(working_dir_, ec);
    if (ec) canonical = working_dir_;
    working_dir_ = canonical;
    loaded_dirs_.insert(working_dir_.string());
}

void SubdirectoryHintDiscoverer::reset_for_testing() {
    loaded_dirs_.clear();
    loaded_dirs_.insert(working_dir_.string());
}

std::optional<std::string> SubdirectoryHintDiscoverer::check_tool_call(
    const std::string& tool_name, const nlohmann::json& tool_args) {
    auto dirs = extract_directories(tool_name, tool_args);
    if (dirs.empty()) return std::nullopt;

    std::vector<std::string> all_hints;
    for (const auto& d : dirs) {
        auto h = load_hints_for_directory(d);
        if (h.has_value()) all_hints.push_back(*h);
    }
    if (all_hints.empty()) return std::nullopt;

    std::string out = "\n\n";
    for (std::size_t i = 0; i < all_hints.size(); ++i) {
        if (i) out += "\n\n";
        out += all_hints[i];
    }
    return out;
}

std::vector<fs::path> SubdirectoryHintDiscoverer::extract_directories(
    const std::string& tool_name, const nlohmann::json& args) {
    std::unordered_set<std::string> keys;
    std::vector<fs::path> out;

    if (args.is_object()) {
        for (const auto& key : path_arg_keys()) {
            auto it = args.find(key);
            if (it != args.end() && it->is_string()) {
                std::string val = strip(it->get<std::string>());
                if (!val.empty()) add_path_candidate(val, keys, out);
            }
        }
        if (is_command_tool(tool_name)) {
            auto it = args.find("command");
            if (it != args.end() && it->is_string()) {
                extract_paths_from_command(it->get<std::string>(), keys, out);
            }
        }
    }
    return out;
}

void SubdirectoryHintDiscoverer::add_path_candidate(
    const std::string& raw_path,
    std::unordered_set<std::string>& out_keys,
    std::vector<fs::path>& out_list) {
    std::error_code ec;
    fs::path p = expanduser(raw_path);
    if (!p.is_absolute()) p = working_dir_ / p;
    fs::path resolved = fs::weakly_canonical(p, ec);
    if (ec) resolved = p;

    // If looks like a file (has suffix, or exists as file), use parent.
    if (!resolved.has_extension() ? false : true) {
        // has_extension => treat as file
        if (resolved.has_extension()) resolved = resolved.parent_path();
    } else if (fs::exists(resolved, ec) && fs::is_regular_file(resolved, ec)) {
        resolved = resolved.parent_path();
    }

    for (int i = 0; i < k_max_ancestor_walk; ++i) {
        if (resolved.empty()) break;
        const std::string key = resolved.string();
        if (loaded_dirs_.count(key)) break;
        if (is_valid_subdir(resolved)) {
            if (out_keys.insert(key).second) {
                out_list.push_back(resolved);
            }
        }
        fs::path parent = resolved.parent_path();
        if (parent == resolved) break;
        resolved = parent;
    }
}

void SubdirectoryHintDiscoverer::extract_paths_from_command(
    const std::string& cmd,
    std::unordered_set<std::string>& out_keys,
    std::vector<fs::path>& out_list) {
    auto tokens = subdir_hints_detail::shell_tokenise(cmd);
    for (const auto& tok : tokens) {
        if (!subdir_hints_detail::looks_like_path_token(tok)) continue;
        add_path_candidate(tok, out_keys, out_list);
    }
}

bool SubdirectoryHintDiscoverer::is_valid_subdir(const fs::path& p) const {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    if (loaded_dirs_.count(p.string())) return false;
    return true;
}

std::string SubdirectoryHintDiscoverer::relative_display_path(
    const fs::path& p) const {
    std::error_code ec;
    fs::path rel = fs::relative(p, working_dir_, ec);
    if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
        return rel.string();
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        fs::path rel_home = fs::relative(p, fs::path(home), ec);
        if (!ec && !rel_home.empty() &&
            rel_home.string().find("..") == std::string::npos) {
            return "~/" + rel_home.string();
        }
    }
    return p.string();
}

std::optional<std::string> SubdirectoryHintDiscoverer::load_hints_for_directory(
    const fs::path& directory) {
    loaded_dirs_.insert(directory.string());

    for (const char* filename : k_hint_filenames) {
        fs::path hint = directory / filename;
        std::error_code ec;
        if (!fs::is_regular_file(hint, ec)) continue;
        std::string content = strip(read_file_to_string(hint));
        if (content.empty()) continue;

        if (content.size() > k_max_hint_chars) {
            std::string trunc(content, 0, k_max_hint_chars);
            trunc += "\n\n[...truncated ";
            trunc += filename;
            trunc += ": ";
            trunc += format_with_commas(content.size());
            trunc += " chars total]";
            content = std::move(trunc);
        }

        const std::string rel = relative_display_path(hint);
        std::string section;
        section += "[Subdirectory context discovered: ";
        section += rel;
        section += "]\n";
        section += content;
        return section;  // first match wins per directory
    }
    return std::nullopt;
}

}  // namespace hermes::agent
