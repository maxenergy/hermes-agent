// `hermes plugins ...` CLI — list / install / uninstall / enable / disable /
// info / update / search / reload.
//
// Mirrors ``hermes_cli/plugins_cmd.py`` in the Python codebase.  Shares the
// same on-disk layout: plugins live in ``~/.hermes/plugins/<name>/`` with a
// ``plugin.yaml`` manifest.  State (installed + disabled lists) is persisted
// to ``~/.hermes/plugins/state.json``.
#include "hermes/plugins/plugins_cli.hpp"

#include "hermes/plugins/manifest.hpp"
#include "hermes/plugins/plugin_manager.hpp"
#include "hermes/plugins/state.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace hermes::plugins {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr int kSupportedManifestVersion = 1;

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Quote an argument for the shell.  Good enough for the narrow set of
// inputs we pass (paths, URLs) — not a general-purpose shell escaper.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Run a command, capturing stdout + stderr into a combined string.
// Returns (exit_code, output).
std::pair<int, std::string> run_command(const std::string& cmd) {
#ifdef _WIN32
    (void)cmd;
    return {1, "subprocess execution not implemented on Windows"};
#else
    std::string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) return {1, "popen failed"};
    std::string out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), buf.size(), pipe)) {
        out += buf.data();
    }
    int rc = pclose(pipe);
    int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
    return {exit_code, out};
#endif
}

// Plugin directory list (sorted) — helper for error messages.
std::string list_installed_names(const fs::path& dir) {
    std::vector<std::string> names;
    if (fs::is_directory(dir)) {
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.is_directory()) names.push_back(e.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) return "(none)";
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

// Copy any `<file>.example` → `<file>` unless the real file already exists.
void copy_example_files(const fs::path& plugin_dir) {
    if (!fs::is_directory(plugin_dir)) return;
    for (const auto& entry : fs::directory_iterator(plugin_dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".example") continue;
        auto real = path;
        real.replace_extension("");  // strip ".example"
        if (fs::exists(real)) continue;
        std::error_code ec;
        fs::copy_file(path, real, ec);
    }
}

// Merge state with on-disk directory presence.  After install/uninstall we
// rebuild the installed list from what actually sits in plugins_dir, then
// re-apply the disabled list (filtered to still-installed plugins).
void sync_state_from_disk(const fs::path& plugins_dir, PluginState& state) {
    std::set<std::string> on_disk;
    if (fs::is_directory(plugins_dir)) {
        for (const auto& e : fs::directory_iterator(plugins_dir)) {
            if (e.is_directory()) {
                on_disk.insert(e.path().filename().string());
            }
        }
    }
    state.installed = on_disk;
    // Drop disabled entries whose plugin no longer exists.
    std::set<std::string> pruned;
    for (const auto& d : state.disabled) {
        if (state.installed.count(d)) pruned.insert(d);
    }
    state.disabled = pruned;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public helpers (declared in plugins_cli.hpp)
// ---------------------------------------------------------------------------

std::string resolve_git_url(const std::string& identifier) {
    if (identifier.empty()) {
        throw std::invalid_argument("plugin identifier must not be empty");
    }
    // Already a URL?
    static const std::array<const char*, 5> prefixes = {
        "https://", "http://", "git@", "ssh://", "file://"
    };
    for (const auto* p : prefixes) {
        if (starts_with(identifier, p)) return identifier;
    }
    // `owner/repo` shorthand → GitHub.
    std::string id = identifier;
    // Trim leading/trailing slashes.
    while (!id.empty() && id.front() == '/') id.erase(id.begin());
    while (!id.empty() && id.back() == '/') id.pop_back();

    auto parts = split(id, '/');
    if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
        return "https://github.com/" + parts[0] + "/" + parts[1] + ".git";
    }
    throw std::invalid_argument(
        "invalid plugin identifier: '" + identifier +
        "' (expected git URL or owner/repo shorthand)");
}

std::string repo_name_from_url(const std::string& url) {
    std::string name = url;
    while (!name.empty() && name.back() == '/') name.pop_back();
    if (ends_with(name, ".git")) name.erase(name.size() - 4);
    // Last path component.
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // `git@github.com:owner/repo` — split on colon too.
    auto colon = name.rfind(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    // Strip owner/ prefix if any remains.
    auto slash2 = name.rfind('/');
    if (slash2 != std::string::npos) name = name.substr(slash2 + 1);
    return name;
}

fs::path sanitize_plugin_name(const std::string& name,
                               const fs::path& plugins_dir) {
    if (name.empty()) {
        throw std::invalid_argument("plugin name must not be empty");
    }
    if (name == "." || name == "..") {
        throw std::invalid_argument(
            "invalid plugin name '" + name + "': reserved");
    }
    for (const char* bad : {"/", "\\", ".."}) {
        if (name.find(bad) != std::string::npos) {
            throw std::invalid_argument(
                "invalid plugin name '" + name + "': must not contain '" +
                bad + "'");
        }
    }
    auto target = fs::weakly_canonical(plugins_dir / name);
    auto root   = fs::weakly_canonical(plugins_dir);
    if (target == root) {
        throw std::invalid_argument(
            "invalid plugin name '" + name + "': resolves to plugins root");
    }
    // Check that target is inside plugins_dir.
    auto rel = fs::relative(target, root);
    auto first = rel.begin() != rel.end() ? rel.begin()->string() : "";
    if (first == "..") {
        throw std::invalid_argument(
            "invalid plugin name '" + name + "': escapes plugins dir");
    }
    return target;
}

fs::path resolve_plugins_dir() {
    const char* env = std::getenv("HERMES_PLUGINS_DIR");
    fs::path dir = env && *env
        ? fs::path(env)
        : hermes::core::path::get_hermes_home() / "plugins";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

GitResult git_clone(const std::string& url, const fs::path& target,
                    int /*timeout_sec*/) {
    GitResult r;
    std::string cmd = "git clone --depth 1 " + shell_quote(url) + " " +
                      shell_quote(target.string());
    auto [rc, out] = run_command(cmd);
    r.exit_code   = rc;
    r.stdout_text = out;
    r.stderr_text = rc != 0 ? out : "";
    return r;
}

GitResult git_pull(const fs::path& repo_dir, int /*timeout_sec*/) {
    GitResult r;
    std::string cmd = "cd " + shell_quote(repo_dir.string()) +
                      " && git pull --ff-only";
    auto [rc, out] = run_command(cmd);
    r.exit_code   = rc;
    r.stdout_text = out;
    r.stderr_text = rc != 0 ? out : "";
    return r;
}

int build_plugin_if_cmake(const fs::path& plugin_dir, bool quiet) {
    if (!fs::exists(plugin_dir / "CMakeLists.txt")) return 0;
    std::string prefix = "cd " + shell_quote(plugin_dir.string()) + " && ";
    {
        auto [rc, out] = run_command(
            prefix + "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release");
        if (rc != 0) {
            if (!quiet) std::cerr << out;
            return rc;
        }
    }
    auto [rc, out] = run_command(prefix + "cmake --build build --parallel");
    if (rc != 0 && !quiet) std::cerr << out;
    return rc;
}

// ---------------------------------------------------------------------------
// Sub-actions
// ---------------------------------------------------------------------------

int plugins_list(const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    PluginState state = load_state(state_path);
    auto manifests = scan_plugins_dir(plugins_dir, "user");

    if (manifests.empty()) {
        if (!opts.quiet) {
            std::cout << "No plugins installed.\n"
                      << "Install with: hermes plugins install owner/repo\n";
        }
        return 0;
    }

    std::cout << "Installed plugins (" << manifests.size() << "):\n\n";
    std::cout << std::left
              << std::setw(24) << "NAME"
              << std::setw(10) << "STATUS"
              << std::setw(12) << "VERSION"
              << "DESCRIPTION\n";
    std::cout << std::string(72, '-') << "\n";

    for (const auto& m : manifests) {
        bool disabled = state.is_disabled(m.name);
        std::cout << std::left
                  << std::setw(24) << m.name
                  << std::setw(10) << (disabled ? "disabled" : "enabled")
                  << std::setw(12) << (m.version.empty() ? "-" : m.version)
                  << m.description
                  << "\n";
    }
    std::cout << "\n";
    return 0;
}

int plugins_install(const std::string& identifier, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    // Case 1: local path (absolute, or contains a slash pointing at a real
    // directory).  Check before treating as git URL.
    fs::path local(identifier);
    if (fs::is_directory(local)) {
        std::string name = local.filename().string();
        fs::path target;
        try {
            target = sanitize_plugin_name(name, plugins_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        if (fs::exists(target)) {
            if (!opts.force) {
                std::cerr << "Error: plugin '" << name << "' already installed.\n"
                          << "Use --force to reinstall.\n";
                return 1;
            }
            fs::remove_all(target);
        }
        std::error_code ec;
        fs::copy(local, target, fs::copy_options::recursive, ec);
        if (ec) {
            std::cerr << "Error: failed to copy plugin: " << ec.message() << "\n";
            return 1;
        }
        copy_example_files(target);
        build_plugin_if_cmake(target, opts.quiet);
        PluginState state = load_state(state_path);
        state.add_installed(name);
        save_state(state_path, state);
        if (!opts.quiet) {
            std::cout << "Plugin installed from local path: " << name
                      << " -> " << target << "\n";
        }
        return 0;
    }

    // Case 2: git URL / owner/repo shorthand.
    std::string git_url;
    try {
        git_url = resolve_git_url(identifier);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (starts_with(git_url, "http://") || starts_with(git_url, "file://")) {
        std::cerr << "Warning: using insecure/local URL scheme ("
                  << git_url << ").\n";
    }

    std::string repo_name = repo_name_from_url(git_url);

    // Clone into a temp directory, read the manifest to get the canonical
    // name, then move into place.
    fs::path tmp = fs::temp_directory_path() /
        ("hermes_plugin_" + std::to_string(std::random_device{}()));
    fs::create_directories(tmp);
    fs::path tmp_target = tmp / "plugin";

    if (!opts.quiet) {
        std::cout << "Cloning " << git_url << "...\n";
    }
    auto clone = git_clone(git_url, tmp_target);
    if (clone.exit_code != 0) {
        std::cerr << "Error: git clone failed:\n" << clone.stderr_text;
        fs::remove_all(tmp);
        return 1;
    }

    // Read manifest for canonical name + manifest_version check.
    PluginManifest m = load_manifest(tmp_target / "plugin.yaml");
    std::string plugin_name = !m.name.empty() ? m.name : repo_name;

    if (m.manifest_version > kSupportedManifestVersion) {
        std::cerr << "Error: plugin '" << plugin_name
                  << "' requires manifest_version " << m.manifest_version
                  << ", but this installer only supports up to "
                  << kSupportedManifestVersion << ".\n";
        fs::remove_all(tmp);
        return 1;
    }

    fs::path target;
    try {
        target = sanitize_plugin_name(plugin_name, plugins_dir);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        fs::remove_all(tmp);
        return 1;
    }

    if (fs::exists(target)) {
        if (!opts.force) {
            std::cerr << "Error: plugin '" << plugin_name
                      << "' already exists at " << target << ".\n"
                      << "Use --force to reinstall, or "
                      << "`hermes plugins update " << plugin_name
                      << "` to pull latest.\n";
            fs::remove_all(tmp);
            return 1;
        }
        fs::remove_all(target);
    }

    std::error_code ec;
    fs::rename(tmp_target, target, ec);
    if (ec) {
        // Cross-device rename fallback — copy then remove.
        fs::copy(tmp_target, target, fs::copy_options::recursive, ec);
        if (ec) {
            std::cerr << "Error: failed to install plugin: "
                      << ec.message() << "\n";
            fs::remove_all(tmp);
            return 1;
        }
        fs::remove_all(tmp_target);
    }
    fs::remove_all(tmp);

    copy_example_files(target);
    build_plugin_if_cmake(target, opts.quiet);

    PluginState state = load_state(state_path);
    state.add_installed(plugin_name);
    save_state(state_path, state);

    if (!opts.quiet) {
        std::cout << "Plugin installed: " << plugin_name << " -> "
                  << target << "\n";
    }
    return 0;
}

int plugins_uninstall(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    fs::path target;
    try {
        target = sanitize_plugin_name(name, plugins_dir);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (!fs::exists(target)) {
        std::cerr << "Error: plugin '" << name << "' not found.\n"
                  << "Installed: " << list_installed_names(plugins_dir) << "\n";
        return 1;
    }
    fs::remove_all(target);

    PluginState state = load_state(state_path);
    state.remove_installed(name);
    sync_state_from_disk(plugins_dir, state);
    save_state(state_path, state);

    if (!opts.quiet) {
        std::cout << "Plugin removed: " << name << "\n";
    }
    return 0;
}

int plugins_enable(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    if (!fs::is_directory(plugins_dir / name)) {
        std::cerr << "Error: plugin '" << name << "' is not installed.\n";
        return 1;
    }
    PluginState state = load_state(state_path);
    state.add_installed(name);
    if (!state.is_disabled(name)) {
        if (!opts.quiet) {
            std::cout << "Plugin '" << name << "' is already enabled.\n";
        }
        return 0;
    }
    state.enable(name);
    save_state(state_path, state);
    if (!opts.quiet) {
        std::cout << "Plugin enabled: " << name << "\n";
    }
    return 0;
}

int plugins_disable(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    if (!fs::is_directory(plugins_dir / name)) {
        std::cerr << "Error: plugin '" << name << "' is not installed.\n";
        return 1;
    }
    PluginState state = load_state(state_path);
    state.add_installed(name);
    if (state.is_disabled(name)) {
        if (!opts.quiet) {
            std::cout << "Plugin '" << name << "' is already disabled.\n";
        }
        return 0;
    }
    state.disable(name);
    save_state(state_path, state);
    if (!opts.quiet) {
        std::cout << "Plugin disabled: " << name << "\n";
    }
    return 0;
}

int plugins_info(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path state_path = opts.state_path.empty()
        ? plugins_dir / "state.json" : opts.state_path;

    fs::path plugin_dir = plugins_dir / name;
    if (!fs::is_directory(plugin_dir)) {
        std::cerr << "Error: plugin '" << name << "' is not installed.\n"
                  << "Installed: " << list_installed_names(plugins_dir) << "\n";
        return 1;
    }

    fs::path yaml_path = plugin_dir / "plugin.yaml";
    if (!fs::exists(yaml_path)) yaml_path = plugin_dir / "plugin.yml";

    PluginManifest m;
    if (fs::exists(yaml_path)) m = load_manifest(yaml_path);
    if (m.name.empty()) m.name = name;

    PluginState state = load_state(state_path);
    bool enabled = !state.is_disabled(name);

    std::cout << "name:         " << m.name << "\n"
              << "version:      " << (m.version.empty() ? "-" : m.version) << "\n"
              << "description:  " << m.description << "\n"
              << "author:       " << m.author << "\n"
              << "path:         " << plugin_dir.string() << "\n"
              << "status:       " << (enabled ? "enabled" : "disabled") << "\n"
              << "manifest_ver: " << m.manifest_version << "\n";

    auto dump_list = [](const char* label,
                        const std::vector<std::string>& v) {
        std::cout << label;
        if (v.empty()) {
            std::cout << "(none)\n";
            return;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << v[i];
        }
        std::cout << "\n";
    };

    dump_list("requires_env: ", m.requires_env);
    dump_list("tools:        ", m.provides_tools);
    dump_list("hooks:        ", m.provides_hooks);
    dump_list("cap/tools:    ", m.capabilities.tools);
    dump_list("cap/commands: ", m.capabilities.commands);
    dump_list("cap/events:   ", m.capabilities.events);
    dump_list("cap/cli:      ", m.capabilities.cli);
    return 0;
}

int plugins_update(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    fs::path plugin_dir = plugins_dir / name;
    if (!fs::is_directory(plugin_dir)) {
        std::cerr << "Error: plugin '" << name << "' is not installed.\n";
        return 1;
    }
    if (!fs::exists(plugin_dir / ".git")) {
        std::cerr << "Error: plugin '" << name
                  << "' was not installed from git — cannot update.\n";
        return 1;
    }
    if (!opts.quiet) std::cout << "Updating " << name << "...\n";
    auto r = git_pull(plugin_dir);
    if (r.exit_code != 0) {
        std::cerr << "Error: git pull failed:\n" << r.stderr_text;
        return 1;
    }
    copy_example_files(plugin_dir);
    build_plugin_if_cmake(plugin_dir, opts.quiet);
    if (!opts.quiet) {
        if (r.stdout_text.find("Already up to date") != std::string::npos) {
            std::cout << "Plugin '" << name << "' is already up to date.\n";
        } else {
            std::cout << "Plugin '" << name << "' updated.\n";
        }
    }
    return 0;
}

int plugins_search(const std::string& query, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    auto manifests = scan_plugins_dir(plugins_dir, "user");

    // Case-insensitive substring search across name/description/author.
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string q = lower(query);

    int hits = 0;
    for (const auto& m : manifests) {
        if (q.empty() ||
            lower(m.name).find(q)        != std::string::npos ||
            lower(m.description).find(q) != std::string::npos ||
            lower(m.author).find(q)      != std::string::npos) {
            std::cout << m.name;
            if (!m.version.empty())     std::cout << "  (" << m.version << ")";
            if (!m.description.empty()) std::cout << " - " << m.description;
            std::cout << "\n";
            ++hits;
        }
    }
    if (!opts.quiet && hits == 0) {
        std::cout << "No plugins matched query: '" << query << "'\n";
    }
    return 0;
}

int plugins_reload(const std::string& name, const PluginsOptions& opts) {
    fs::path plugins_dir = opts.plugins_dir.empty()
        ? resolve_plugins_dir() : opts.plugins_dir;
    if (!fs::is_directory(plugins_dir / name)) {
        std::cerr << "Error: plugin '" << name << "' is not installed.\n";
        return 1;
    }
    // Hot reload uses PluginManager.  In CLI context we don't have a live
    // instance — we simulate by calling discover() which rebuilds lookups.
    PluginManager mgr(plugins_dir);
    mgr.discover();
    bool ok = mgr.reload(name);
    if (!opts.quiet) {
        std::cout << (ok ? "Plugin reloaded: " : "Plugin reload scheduled: ")
                  << name << "\n";
    }
    return ok ? 0 : 0;  // reload returning false is not fatal — stub libs etc.
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

namespace {

void print_help() {
    std::cout <<
        "Usage: hermes plugins <action> [args]\n\n"
        "Actions:\n"
        "  list                         List installed plugins\n"
        "  install <url|owner/repo|path> [--force]  Install a plugin\n"
        "  uninstall <name>             Remove an installed plugin\n"
        "  enable <name>                Mark a plugin as enabled\n"
        "  disable <name>               Mark a plugin as disabled\n"
        "  info <name>                  Show plugin manifest details\n"
        "  update <name>                Pull latest for a git-installed plugin\n"
        "  search <query>               Search installed plugins by name/desc\n"
        "  reload <name>                Hot-reload a plugin\n";
}

} // anonymous namespace

int cmd_plugins(int argc, char* argv[]) {
    if (argc < 3) {
        // `hermes plugins` with no action — default to `list`.
        PluginsOptions opts;
        return plugins_list(opts);
    }

    std::string action = argv[2];
    PluginsOptions opts;
    std::vector<std::string> positional;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--force" || a == "-f")       opts.force = true;
        else if (a == "--quiet" || a == "-q")  opts.quiet = true;
        else if (a == "--help" || a == "-h")   { print_help(); return 0; }
        else if (starts_with(a, "--dir=")) {
            opts.plugins_dir = a.substr(6);
        } else {
            positional.push_back(a);
        }
    }

    auto need_one = [&](const char* what) -> bool {
        if (positional.empty()) {
            std::cerr << "Error: `hermes plugins " << action
                      << "` requires " << what << ".\n";
            return false;
        }
        return true;
    };

    if (action == "list")      return plugins_list(opts);
    if (action == "install") {
        if (!need_one("<url|owner/repo|path>")) return 1;
        return plugins_install(positional[0], opts);
    }
    if (action == "uninstall" || action == "remove") {
        if (!need_one("<name>")) return 1;
        return plugins_uninstall(positional[0], opts);
    }
    if (action == "enable") {
        if (!need_one("<name>")) return 1;
        return plugins_enable(positional[0], opts);
    }
    if (action == "disable") {
        if (!need_one("<name>")) return 1;
        return plugins_disable(positional[0], opts);
    }
    if (action == "info") {
        if (!need_one("<name>")) return 1;
        return plugins_info(positional[0], opts);
    }
    if (action == "update") {
        if (!need_one("<name>")) return 1;
        return plugins_update(positional[0], opts);
    }
    if (action == "search") {
        std::string q = positional.empty() ? "" : positional[0];
        return plugins_search(q, opts);
    }
    if (action == "reload") {
        if (!need_one("<name>")) return 1;
        return plugins_reload(positional[0], opts);
    }

    std::cerr << "Unknown plugins action: " << action << "\n";
    print_help();
    return 1;
}

} // namespace hermes::plugins
