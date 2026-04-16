// main_update_flow — self-update orchestration.  See header for API.

#include "hermes/cli/main_update_flow.hpp"
#include "hermes/core/path.hpp"
#include "hermes/core/platform/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hermes::cli::update_flow {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Real shell
// ---------------------------------------------------------------------------
namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

RealGitShell::RealGitShell(std::string git_binary) : git_(std::move(git_binary)) {}

GitResult RealGitShell::run(const std::vector<std::string>& args,
                            const std::string& cwd) {
    GitResult r;
    hermes::core::platform::SubprocessOptions opts;
    opts.argv.reserve(args.size() + 1);
    opts.argv.push_back(git_);
    for (const auto& a : args) opts.argv.push_back(a);
    opts.cwd = cwd;
    auto res = hermes::core::platform::run_capture(opts);
    if (!res.spawn_error.empty()) {
        r.exit_code = -1;
        return r;
    }
    r.stdout_text = std::move(res.stdout_text);
    r.exit_code = res.exit_code;
    return r;
}

// ---------------------------------------------------------------------------
// Fake shell
// ---------------------------------------------------------------------------
GitResult FakeGitShell::run(const std::vector<std::string>& args,
                            const std::string& cwd) {
    calls_.push_back({args, cwd});
    if (results_.empty()) {
        return GitResult{};  // default: exit 0, empty output
    }
    auto r = results_.front();
    results_.erase(results_.begin());
    return r;
}

// ---------------------------------------------------------------------------
// Bytecode cache
// ---------------------------------------------------------------------------
int clear_bytecode_cache(const std::string& root) {
    static const std::vector<std::string> skip = {
        "venv", ".venv", "node_modules", ".git", ".worktrees",
        "build", "cpp/build", "target",
    };
    int removed = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    // Walk the tree — manual recursion so we can prune.
    std::vector<fs::path> stack{fs::path(root)};
    while (!stack.empty()) {
        auto dir = stack.back();
        stack.pop_back();
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; ++it) {
            if (ec) break;
            if (!it->is_directory(ec)) continue;
            std::string name = it->path().filename().string();
            bool skip_this =
                std::find(skip.begin(), skip.end(), name) != skip.end();
            if (skip_this) continue;
            if (name == "__pycache__") {
                fs::remove_all(it->path(), ec);
                if (!ec) ++removed;
                continue;
            }
            stack.push_back(it->path());
        }
    }
    return removed;
}

bool invalidate_update_cache() {
    try {
        auto path = hermes::core::path::get_hermes_home() / ".update_cache";
        std::error_code ec;
        if (fs::exists(path, ec)) {
            fs::remove(path, ec);
            return !ec;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Gateway prompt — file-based IPC
// ---------------------------------------------------------------------------
std::string gateway_prompt(const std::string& prompt_text,
                           const std::string& default_val,
                           double timeout_secs) {
    try {
        auto home = hermes::core::path::get_hermes_home();
        auto prompt_path   = home / ".update_prompt.json";
        auto response_path = home / ".update_response";
        std::error_code ec;
        fs::remove(response_path, ec);

        nlohmann::json payload;
        payload["prompt"] = prompt_text;
        payload["default"] = default_val;
        // Generate a pseudo-uuid — not cryptographic, just a correlator.
        char id[16] = {0};
        std::snprintf(id, sizeof(id), "u-%08x",
                      static_cast<unsigned>(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count() &
                                            0xFFFFFFFFu));
        payload["id"] = std::string(id);

        auto tmp = prompt_path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp);
            f << payload.dump();
        }
        fs::rename(tmp, prompt_path, ec);

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            static_cast<int>(timeout_secs * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            if (fs::exists(response_path, ec)) {
                std::ifstream f(response_path);
                std::ostringstream buf;
                buf << f.rdbuf();
                auto answer = trim(buf.str());
                fs::remove(response_path, ec);
                fs::remove(prompt_path, ec);
                return answer.empty() ? default_val : answer;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        fs::remove(prompt_path, ec);
        fs::remove(response_path, ec);
        return default_val;
    } catch (...) {
        return default_val;
    }
}

// ---------------------------------------------------------------------------
// Git helpers
// ---------------------------------------------------------------------------
std::optional<std::string> get_origin_url(GitShell& sh, const std::string& cwd) {
    auto r = sh.run({"config", "--get", "remote.origin.url"}, cwd);
    if (r.exit_code != 0) return std::nullopt;
    auto t = trim(r.stdout_text);
    if (t.empty()) return std::nullopt;
    return t;
}

bool is_fork(const std::optional<std::string>& origin_url) {
    if (!origin_url) return false;
    const auto& u = *origin_url;
    // Heuristic: NousResearch/hermes-agent canonical; anything else that
    // still references "hermes-agent" is likely a fork.
    if (u.find("NousResearch/hermes-agent") != std::string::npos) return false;
    return u.find("hermes-agent") != std::string::npos;
}

bool has_upstream_remote(GitShell& sh, const std::string& cwd) {
    auto r = sh.run({"remote"}, cwd);
    if (r.exit_code != 0) return false;
    std::istringstream is(r.stdout_text);
    std::string line;
    while (std::getline(is, line)) {
        if (trim(line) == "upstream") return true;
    }
    return false;
}

bool add_upstream_remote(GitShell& sh, const std::string& cwd) {
    auto r = sh.run(
        {"remote", "add", "upstream",
         "https://github.com/NousResearch/hermes-agent.git"},
        cwd);
    return r.exit_code == 0;
}

int count_commits_between(GitShell& sh, const std::string& cwd,
                          const std::string& base, const std::string& head) {
    auto r = sh.run({"rev-list", "--count", base + ".." + head}, cwd);
    if (r.exit_code != 0) return -1;
    try {
        return std::stoi(trim(r.stdout_text));
    } catch (...) {
        return -1;
    }
}

namespace {
fs::path skip_upstream_marker() {
    return hermes::core::path::get_hermes_home() /
           ".update_skip_upstream_prompt";
}
}

bool should_skip_upstream_prompt() {
    std::error_code ec;
    return fs::exists(skip_upstream_marker(), ec);
}

void mark_skip_upstream_prompt() {
    try {
        auto p = skip_upstream_marker();
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << "skip";
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Stash helpers
// ---------------------------------------------------------------------------
std::optional<std::string> stash_local_changes_if_needed(GitShell& sh,
                                                          const std::string& cwd) {
    // `git status --porcelain` — empty output means clean working tree.
    auto st = sh.run({"status", "--porcelain"}, cwd);
    if (st.exit_code != 0) return std::nullopt;
    if (trim(st.stdout_text).empty()) return std::nullopt;

    // Push stash.
    auto push = sh.run({"stash", "push", "-u", "-m", "hermes update stash"}, cwd);
    if (push.exit_code != 0) return std::nullopt;

    // Resolve to the canonical stash@{0} ref.
    return std::string("stash@{0}");
}

std::optional<std::string> resolve_stash_selector(GitShell& sh,
                                                  const std::string& cwd,
                                                  const std::string& stash_ref) {
    auto r = sh.run({"rev-parse", stash_ref}, cwd);
    if (r.exit_code != 0) return std::nullopt;
    auto sha = trim(r.stdout_text);
    if (sha.empty()) return std::nullopt;
    return sha;
}

bool restore_stashed_changes(GitShell& sh, const std::string& cwd,
                             const std::string& stash_ref,
                             std::vector<std::string>* conflicts) {
    auto r = sh.run({"stash", "pop", stash_ref}, cwd);
    if (r.exit_code == 0) return true;

    // Parse conflict lines from stdout — best-effort.
    if (conflicts) {
        std::istringstream is(r.stdout_text);
        std::string line;
        while (std::getline(is, line)) {
            if (line.rfind("CONFLICT", 0) == 0 || line.rfind("Merge conflict", 0) == 0) {
                conflicts->push_back(line);
            }
        }
    }
    return false;
}

std::string format_stash_cleanup_guidance(
    const std::string& stash_ref,
    const std::optional<std::string>& selector) {
    std::ostringstream o;
    o << "\nYour local changes are preserved in " << stash_ref
      << ".  Resolve conflicts, then run:\n\n"
      << "    git stash pop " << stash_ref << "\n";
    if (selector) {
        o << "\nThe stash commit is " << *selector << ".\n";
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// Upstream sync
// ---------------------------------------------------------------------------
bool sync_fork_with_upstream(GitShell& sh, const std::string& cwd) {
    auto fetch = sh.run({"fetch", "upstream"}, cwd);
    if (fetch.exit_code != 0) return false;
    auto merge = sh.run({"merge", "--ff-only", "upstream/main"}, cwd);
    return merge.exit_code == 0;
}

bool sync_with_upstream_if_needed(
    GitShell& sh, const std::string& cwd,
    std::function<std::string(const std::string&)> prompt) {
    if (should_skip_upstream_prompt()) return false;
    auto origin = get_origin_url(sh, cwd);
    if (!is_fork(origin)) return false;
    if (!has_upstream_remote(sh, cwd)) {
        if (!add_upstream_remote(sh, cwd)) return false;
    }
    bool go = true;
    if (prompt) {
        auto answer = prompt("Sync fork with upstream main? [Y/n] ");
        if (!answer.empty() &&
            (answer[0] == 'n' || answer[0] == 'N')) {
            go = false;
        }
    }
    if (!go) return false;
    return sync_fork_with_upstream(sh, cwd);
}

// ---------------------------------------------------------------------------
// run_update — top-level orchestrator
// ---------------------------------------------------------------------------
namespace {

UpdateResult run_update_impl(const UpdateOptions& opts, GitShell& sh) {
    UpdateResult r;
    std::string cwd = opts.working_dir;
    if (cwd.empty()) cwd = fs::current_path().string();

    // 1. Validate git checkout.
    auto toplevel = sh.run({"rev-parse", "--show-toplevel"}, cwd);
    if (toplevel.exit_code != 0) {
        r.exit_code = 1;
        r.summary = "Not a git checkout — cannot self-update";
        return r;
    }
    std::string repo_root = trim(toplevel.stdout_text);
    if (repo_root.empty()) repo_root = cwd;

    // 2. Optional fork sync.
    (void)sync_with_upstream_if_needed(sh, repo_root, nullptr);

    // 3. Stash local changes.
    r.stash_ref = stash_local_changes_if_needed(sh, repo_root);

    // 4. Fetch + fast-forward.
    auto fetch = sh.run({"fetch", "origin"}, repo_root);
    if (fetch.exit_code != 0) {
        r.exit_code = 2;
        r.summary = "git fetch failed";
        return r;
    }
    // Count commits behind.
    auto behind = sh.run(
        {"rev-list", "--count",
         "HEAD.." + std::string("origin/") + opts.branch},
        repo_root);
    if (behind.exit_code == 0) {
        try {
            r.commits_pulled = std::stoi(trim(behind.stdout_text));
        } catch (...) {
            r.commits_pulled = 0;
        }
    }
    if (r.commits_pulled == 0 && !opts.force) {
        r.summary = "Already up to date";
        // Still restore stash if we made one.
        if (r.stash_ref) {
            restore_stashed_changes(sh, repo_root, *r.stash_ref, &r.conflicts);
        }
        return r;
    }
    if (opts.dry_run) {
        r.summary = std::to_string(r.commits_pulled) +
                    " commits would be pulled (dry-run)";
        if (r.stash_ref) {
            restore_stashed_changes(sh, repo_root, *r.stash_ref, &r.conflicts);
        }
        return r;
    }
    auto merge = sh.run(
        {"merge", "--ff-only", std::string("origin/") + opts.branch},
        repo_root);
    if (merge.exit_code != 0) {
        r.exit_code = 3;
        r.summary = "git merge --ff-only failed";
        return r;
    }
    r.updated = true;

    // 5. Restore stash.
    if (r.stash_ref) {
        if (!restore_stashed_changes(sh, repo_root, *r.stash_ref,
                                      &r.conflicts)) {
            r.summary = format_stash_cleanup_guidance(*r.stash_ref);
        }
    }

    // 6. Clear bytecode cache + update cache.
    clear_bytecode_cache(repo_root);
    invalidate_update_cache();

    if (r.summary.empty()) {
        r.summary = "Updated (" + std::to_string(r.commits_pulled) +
                    " commits pulled)";
    }
    return r;
}

}  // namespace

UpdateResult run_update(const UpdateOptions& opts, GitShell& sh) {
    return run_update_impl(opts, sh);
}

UpdateResult run_update(const UpdateOptions& opts) {
    RealGitShell sh;
    return run_update_impl(opts, sh);
}

}  // namespace hermes::cli::update_flow
