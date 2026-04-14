// main_update_flow — self-update orchestration.
//
// Ports the following Python helpers from hermes_cli/main.py:
//   _clear_bytecode_cache(root)
//   _gateway_prompt(prompt, default, timeout)
//   _update_via_zip(args)
//   _stash_local_changes_if_needed(git_cmd, cwd)
//   _resolve_stash_selector(git_cmd, cwd, stash_ref)
//   _print_stash_cleanup_guidance(...)
//   _restore_stashed_changes(...)
//   _get_origin_url(git_cmd, cwd)
//   _is_fork(origin_url)
//   _has_upstream_remote(git_cmd, cwd)
//   _add_upstream_remote(git_cmd, cwd)
//   _count_commits_between(git_cmd, cwd, base, head)
//   _should_skip_upstream_prompt()
//   _mark_skip_upstream_prompt()
//   _sync_fork_with_upstream(git_cmd, cwd)
//   _sync_with_upstream_if_needed(git_cmd, cwd)
//   _invalidate_update_cache()
//   cmd_update(args)
//
// The C++ port is intentionally simpler: it shells out to `git` for all
// VCS operations rather than re-implementing them.  Tests stub out the
// command runner via GitShell so they can exercise the orchestration
// logic without touching an actual git repo.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hermes::cli::update_flow {

// ---------------------------------------------------------------------------
// GitShell — thin abstraction over `git` invocations.  Tests inject a fake.
// ---------------------------------------------------------------------------
struct GitResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

class GitShell {
   public:
    virtual ~GitShell() = default;
    virtual GitResult run(const std::vector<std::string>& args,
                          const std::string& cwd) = 0;
};

// Default implementation that shells out to `git` via popen.
class RealGitShell : public GitShell {
   public:
    explicit RealGitShell(std::string git_binary = "git");
    GitResult run(const std::vector<std::string>& args,
                  const std::string& cwd) override;

   private:
    std::string git_;
};

// Fake shell for tests — records calls and returns scripted results.
class FakeGitShell : public GitShell {
   public:
    struct Call {
        std::vector<std::string> args;
        std::string cwd;
    };

    void push_result(const GitResult& r) { results_.push_back(r); }
    void push_result(int code, std::string out = "", std::string err = "") {
        results_.push_back({code, std::move(out), std::move(err)});
    }
    const std::vector<Call>& calls() const { return calls_; }
    GitResult run(const std::vector<std::string>& args,
                  const std::string& cwd) override;

   private:
    std::vector<GitResult> results_;
    std::vector<Call> calls_;
};

// ---------------------------------------------------------------------------
// Bytecode cache — walks `root`, removes __pycache__ directories.  Returns
// the number of directories removed.  Skips venv / .venv / node_modules /
// .git / .worktrees.
// ---------------------------------------------------------------------------
int clear_bytecode_cache(const std::string& root);

// Invalidate `<HERMES_HOME>/.update_cache`.  Returns true on success.
bool invalidate_update_cache();

// ---------------------------------------------------------------------------
// Gateway prompt — file-based IPC between hermes CLI and gateway.  When the
// CLI needs user input during `hermes update --gateway`, it writes a marker
// file to HERMES_HOME and polls for a response.  Used for prompts like
// "restore stashed changes? [y/N]".
//
// Returns the user's response text, or `default` on timeout / error.
// ---------------------------------------------------------------------------
std::string gateway_prompt(const std::string& prompt_text,
                           const std::string& default_val = "",
                           double timeout_secs = 300.0);

// ---------------------------------------------------------------------------
// Git helpers — each is a direct port of the like-named Python helper.
// ---------------------------------------------------------------------------
std::optional<std::string> get_origin_url(GitShell& sh, const std::string& cwd);
bool is_fork(const std::optional<std::string>& origin_url);
bool has_upstream_remote(GitShell& sh, const std::string& cwd);
bool add_upstream_remote(GitShell& sh, const std::string& cwd);
int count_commits_between(GitShell& sh, const std::string& cwd,
                          const std::string& base, const std::string& head);

// The skip-prompt file lives at <HERMES_HOME>/.update_skip_upstream_prompt.
bool should_skip_upstream_prompt();
void mark_skip_upstream_prompt();

// Returns the stash ref (for example "stash@{0}") that was created, or
// std::nullopt if nothing needed to be stashed.
std::optional<std::string> stash_local_changes_if_needed(
    GitShell& sh, const std::string& cwd);

// Restore a previously created stash.  Returns true on clean application.
// On conflict the stash is kept and `conflicts` is populated with the
// offending file paths.
bool restore_stashed_changes(GitShell& sh, const std::string& cwd,
                             const std::string& stash_ref,
                             std::vector<std::string>* conflicts = nullptr);

// Print user-facing guidance when the stash cleanup went wrong.
std::string format_stash_cleanup_guidance(
    const std::string& stash_ref,
    const std::optional<std::string>& selector = std::nullopt);

// Resolve a stash ref (like "stash@{0}") to its commit SHA.  Returns
// std::nullopt if the ref is missing.
std::optional<std::string> resolve_stash_selector(GitShell& sh,
                                                  const std::string& cwd,
                                                  const std::string& stash_ref);

// Sync a fork with upstream — if the remote exists, fast-forward main.
// Returns true on success, false on conflict / network error.
bool sync_fork_with_upstream(GitShell& sh, const std::string& cwd);

// Wrapper — shows a prompt (unless skip marker is set) and calls
// sync_fork_with_upstream on a positive reply.
bool sync_with_upstream_if_needed(
    GitShell& sh, const std::string& cwd,
    std::function<std::string(const std::string&)> prompt);

// ---------------------------------------------------------------------------
// Update command entry — ports `cmd_update(args)`.  Runs the full flow:
//   1) Validate we're inside a git checkout
//   2) Optional fork-upstream sync
//   3) Stash local changes if dirty
//   4) git fetch + fast-forward merge
//   5) Restore stash
//   6) Clear bytecode cache + invalidate update cache
// Returns an exit code (0 on success).
// ---------------------------------------------------------------------------
struct UpdateOptions {
    bool dry_run = false;
    bool force   = false;
    bool use_zip = false;           // Windows fallback
    bool gateway_mode = false;      // prompt via file IPC
    std::string working_dir;        // default: hermes project root
    std::string branch = "main";
};

struct UpdateResult {
    int exit_code = 0;
    bool updated = false;           // fast-forward happened
    int commits_pulled = 0;
    std::optional<std::string> stash_ref;
    std::vector<std::string> conflicts;
    std::string summary;            // human-readable
};

UpdateResult run_update(const UpdateOptions& opts, GitShell& sh);
UpdateResult run_update(const UpdateOptions& opts);  // uses RealGitShell

}  // namespace hermes::cli::update_flow
