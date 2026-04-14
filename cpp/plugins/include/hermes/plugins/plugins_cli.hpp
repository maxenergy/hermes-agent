// `hermes plugins ...` CLI dispatcher.  Parses argv and routes to the
// appropriate sub-action.  All sub-actions share the plugins directory
// (``~/.hermes/plugins/``) and state.json.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hermes::plugins {

/// Options gathered from the plugins subcommand argv.
struct PluginsOptions {
    std::filesystem::path plugins_dir;   // overrides default ~/.hermes/plugins
    std::filesystem::path state_path;    // overrides default state.json
    bool force = false;                  // for install
    bool quiet = false;                  // suppress non-error output
};

/// Entry point for `hermes plugins ...`.  Returns an exit code (0 = ok).
int cmd_plugins(int argc, char* argv[]);

// ---- Individual sub-actions (exposed for unit tests) --------------------

int plugins_list(const PluginsOptions& opts);
int plugins_install(const std::string& identifier, const PluginsOptions& opts);
int plugins_uninstall(const std::string& name, const PluginsOptions& opts);
int plugins_enable(const std::string& name, const PluginsOptions& opts);
int plugins_disable(const std::string& name, const PluginsOptions& opts);
int plugins_info(const std::string& name, const PluginsOptions& opts);
int plugins_update(const std::string& name, const PluginsOptions& opts);
int plugins_search(const std::string& query, const PluginsOptions& opts);
int plugins_reload(const std::string& name, const PluginsOptions& opts);

// ---- Helpers (exposed for unit tests) -----------------------------------

/// Turn an identifier (full git URL or owner/repo shorthand) into a git
/// URL that `git clone` will accept.  Throws std::invalid_argument on
/// a malformed identifier.
std::string resolve_git_url(const std::string& identifier);

/// Extract a plugin directory name from a git URL.
std::string repo_name_from_url(const std::string& url);

/// Validate a plugin name against path-traversal.  Returns the resolved
/// target path inside @p plugins_dir.  Throws std::invalid_argument on
/// any rejected input (empty, "..", contains '/', resolves outside dir).
std::filesystem::path sanitize_plugin_name(const std::string& name,
                                            const std::filesystem::path& plugins_dir);

/// Resolve plugins directory for the active profile.  If the env override
/// `HERMES_PLUGINS_DIR` is set, use it.  Otherwise
/// ``<hermes_home>/plugins``.  Creates the directory if missing.
std::filesystem::path resolve_plugins_dir();

/// Invoke `git clone` into @p target.  Returns (exit_code, stderr).
struct GitResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};
GitResult git_clone(const std::string& url,
                    const std::filesystem::path& target,
                    int timeout_sec = 60);
GitResult git_pull(const std::filesystem::path& repo_dir,
                   int timeout_sec = 60);

/// If @p plugin_dir contains a CMakeLists.txt, run `cmake -S . -B build`
/// and `cmake --build build`.  Returns the cmake exit code (0 if no
/// CMakeLists.txt, i.e. nothing to do).
int build_plugin_if_cmake(const std::filesystem::path& plugin_dir,
                          bool quiet = false);

} // namespace hermes::plugins
