// Full C++17 port of hermes_cli/profiles.py.
//
// Function-by-function parity with the Python source.  All path
// resolution is HOME-anchored (see header invariant).
#include "hermes/profile/profile.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hermes::profile {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const std::vector<std::string>& profile_dirs() {
    static const std::vector<std::string> kDirs = {
        "memories", "sessions", "skills",   "skins", "logs",
        "plans",    "workspace", "cron",    "home",
    };
    return kDirs;
}

const std::vector<std::string>& clone_config_files() {
    static const std::vector<std::string> kFiles = {
        "config.yaml", ".env", "SOUL.md",
    };
    return kFiles;
}

const std::vector<std::string>& clone_subdir_files() {
    static const std::vector<std::string> kFiles = {
        "memories/MEMORY.md",
        "memories/USER.md",
    };
    return kFiles;
}

const std::vector<std::string>& clone_all_strip() {
    static const std::vector<std::string> kFiles = {
        "gateway.pid",
        "gateway_state.json",
        "processes.json",
    };
    return kFiles;
}

bool is_default_export_excluded_root(std::string_view entry) {
    static const std::unordered_set<std::string> kSet = {
        "hermes-agent", ".worktrees", "profiles", "bin", "node_modules",
        "state.db", "state.db-shm", "state.db-wal",
        "hermes_state.db",
        "response_store.db", "response_store.db-shm", "response_store.db-wal",
        "gateway.pid", "gateway_state.json", "processes.json",
        "auth.json", ".env", "auth.lock", "active_profile", ".update_check",
        "errors.log", ".hermes_history",
        "image_cache", "audio_cache", "document_cache",
        "browser_screenshots", "checkpoints",
        "sandboxes", "logs",
    };
    return kSet.count(std::string(entry)) > 0;
}

bool is_reserved_name(std::string_view name) {
    static const std::unordered_set<std::string> kSet = {
        "hermes", "default", "test", "tmp", "root", "sudo",
    };
    return kSet.count(std::string(name)) > 0;
}

bool is_hermes_subcommand(std::string_view name) {
    static const std::unordered_set<std::string> kSet = {
        "chat", "model", "gateway", "setup", "whatsapp", "login", "logout",
        "status", "cron", "doctor", "dump", "config", "pairing", "skills",
        "tools", "mcp", "sessions", "insights", "version", "update",
        "uninstall", "profile", "plugins", "honcho", "acp",
    };
    return kSet.count(std::string(name)) > 0;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

fs::path get_profiles_root() {
    return hermes::core::path::get_profiles_root();
}

fs::path get_default_hermes_home() {
    return hermes::core::path::get_default_hermes_root();
}

fs::path get_active_profile_path() {
    return get_default_hermes_home() / "active_profile";
}

fs::path get_wrapper_dir() {
    const char* home_env = std::getenv("HOME");
    fs::path home = (home_env && *home_env) ? fs::path(home_env) : fs::path("/");
    return home / ".local" / "bin";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool is_valid_profile_name(std::string_view name) {
    if (name == "default") return true;
    if (name.empty() || name.size() > 64) return false;
    const char c0 = name[0];
    const bool first_ok =
        (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9');
    if (!first_ok) return false;
    for (size_t i = 1; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void validate_profile_name(std::string_view name) {
    if (!is_valid_profile_name(name)) {
        throw std::invalid_argument(
            std::string("Invalid profile name '") + std::string(name) +
            "'. Must match [a-z0-9][a-z0-9_-]{0,63}");
    }
}

fs::path get_profile_dir(std::string_view name) {
    if (name == "default") {
        return get_default_hermes_home();
    }
    return get_profiles_root() / std::string(name);
}

bool profile_exists(std::string_view name) {
    if (name == "default") return true;
    std::error_code ec;
    return fs::is_directory(get_profile_dir(name), ec);
}

// ---------------------------------------------------------------------------
// Alias / wrapper script management
// ---------------------------------------------------------------------------

namespace {

std::string read_file_safe(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.good()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string which(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::string path_str(path_env);
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(sep, start);
        if (end == std::string::npos) end = path_str.size();
        std::string dir = path_str.substr(start, end - start);
        if (!dir.empty()) {
            fs::path candidate = fs::path(dir) / std::string(name);
            std::error_code ec;
            if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
                return candidate.string();
            }
        }
        if (end == path_str.size()) break;
        start = end + 1;
    }
    return {};
}

}  // namespace

std::string check_alias_collision(std::string_view name) {
    if (is_reserved_name(name)) {
        return std::string("'") + std::string(name) + "' is a reserved name";
    }
    if (is_hermes_subcommand(name)) {
        return std::string("'") + std::string(name) +
               "' conflicts with a hermes subcommand";
    }
    const auto wrapper_dir = get_wrapper_dir();
    const std::string existing = which(name);
    if (!existing.empty()) {
        const auto our_wrapper = (wrapper_dir / std::string(name)).string();
        if (existing == our_wrapper) {
            const std::string content = read_file_safe(wrapper_dir / std::string(name));
            if (content.find("hermes -p") != std::string::npos) {
                return {};
            }
        }
        return std::string("'") + std::string(name) +
               "' conflicts with an existing command (" + existing + ")";
    }
    return {};
}

bool is_wrapper_dir_in_path() {
    const auto wrapper_dir = get_wrapper_dir().string();
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::string path_str(path_env);
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(sep, start);
        if (end == std::string::npos) end = path_str.size();
        if (path_str.substr(start, end - start) == wrapper_dir) return true;
        if (end == path_str.size()) break;
        start = end + 1;
    }
    return false;
}

fs::path create_wrapper_script(std::string_view name) {
    const auto wrapper_dir = get_wrapper_dir();
    std::error_code ec;
    fs::create_directories(wrapper_dir, ec);
    if (ec) {
        std::cerr << "warn: could not create " << wrapper_dir << ": "
                  << ec.message() << "\n";
        return {};
    }
    const auto wrapper_path = wrapper_dir / std::string(name);
    try {
        std::ofstream f(wrapper_path, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            std::cerr << "warn: could not create wrapper at " << wrapper_path
                      << "\n";
            return {};
        }
        f << "#!/bin/sh\nexec hermes -p " << std::string(name) << " \"$@\"\n";
        f.close();
#if defined(__unix__) || defined(__APPLE__)
        struct stat st{};
        if (::stat(wrapper_path.c_str(), &st) == 0) {
            ::chmod(wrapper_path.c_str(),
                    st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
#endif
        return wrapper_path;
    } catch (const std::exception& e) {
        std::cerr << "warn: could not create wrapper at " << wrapper_path
                  << ": " << e.what() << "\n";
        return {};
    }
}

bool remove_wrapper_script(std::string_view name) {
    const auto wrapper_path = get_wrapper_dir() / std::string(name);
    std::error_code ec;
    if (!fs::exists(wrapper_path, ec)) return false;
    const std::string content = read_file_safe(wrapper_path);
    if (content.find("hermes -p") == std::string::npos) {
        return false;  // not ours
    }
    fs::remove(wrapper_path, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// ProfileInfo helpers
// ---------------------------------------------------------------------------

namespace {

std::string strip_inline(const std::string& s) {
    std::string out = s;
    size_t start = out.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    out = out.substr(start);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                            out.back() == '\r' || out.back() == '\n')) {
        out.pop_back();
    }
    return out;
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

void read_config_model(const fs::path& profile_dir, std::string& out_model,
                       std::string& out_provider) {
    out_model.clear();
    out_provider.clear();
    const auto cfg_path = profile_dir / "config.yaml";
    std::error_code ec;
    if (!fs::exists(cfg_path, ec)) return;
    std::ifstream f(cfg_path);
    if (!f.good()) return;
    std::string line;
    bool in_model_block = false;
    while (std::getline(f, line)) {
        std::string s = line;
        if (auto h = s.find('#'); h != std::string::npos) s.erase(h);
        if (s.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        size_t indent = 0;
        while (indent < s.size() && (s[indent] == ' ' || s[indent] == '\t'))
            ++indent;
        std::string trimmed = s.substr(indent);
        while (!trimmed.empty() &&
               (trimmed.back() == ' ' || trimmed.back() == '\r' ||
                trimmed.back() == '\t' || trimmed.back() == '\n'))
            trimmed.pop_back();
        if (indent == 0) {
            in_model_block = false;
            if (trimmed.rfind("model:", 0) == 0) {
                std::string value = strip_inline(trimmed.substr(6));
                if (value.empty()) {
                    in_model_block = true;
                    continue;
                }
                out_model = strip_quotes(value);
                return;
            }
        } else if (in_model_block) {
            if (trimmed.rfind("default:", 0) == 0 && out_model.empty()) {
                out_model = strip_quotes(strip_inline(trimmed.substr(8)));
            } else if (trimmed.rfind("model:", 0) == 0 && out_model.empty()) {
                out_model = strip_quotes(strip_inline(trimmed.substr(6)));
            } else if (trimmed.rfind("provider:", 0) == 0) {
                out_provider = strip_quotes(strip_inline(trimmed.substr(9)));
            }
        }
    }
}

bool check_gateway_running(const fs::path& profile_dir) {
    const auto pid_file = profile_dir / "gateway.pid";
    std::error_code ec;
    if (!fs::exists(pid_file, ec)) return false;
    std::string raw = read_file_safe(pid_file);
    while (!raw.empty() &&
           (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\n'))
        raw.erase(raw.begin());
    while (!raw.empty() &&
           (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\n' ||
            raw.back() == '\r'))
        raw.pop_back();
    if (raw.empty()) return false;
    long pid = 0;
    if (raw.front() == '{') {
        auto p = raw.find("\"pid\"");
        if (p == std::string::npos) return false;
        p = raw.find(':', p);
        if (p == std::string::npos) return false;
        ++p;
        while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p])))
            ++p;
        size_t end = p;
        while (end < raw.size() &&
               std::isdigit(static_cast<unsigned char>(raw[end])))
            ++end;
        if (end == p) return false;
        try {
            pid = std::stol(raw.substr(p, end - p));
        } catch (...) {
            return false;
        }
    } else {
        try {
            pid = std::stol(raw);
        } catch (...) {
            return false;
        }
    }
    if (pid <= 0) return false;
#if defined(__unix__) || defined(__APPLE__)
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
#else
    return false;
#endif
}

int count_skills(const fs::path& profile_dir) {
    const auto skills_dir = profile_dir / "skills";
    std::error_code ec;
    if (!fs::is_directory(skills_dir, ec)) return 0;
    int count = 0;
    for (auto it = fs::recursive_directory_iterator(
             skills_dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        std::error_code ec2;
        if (entry.is_regular_file(ec2) && !ec2 &&
            entry.path().filename() == "SKILL.md") {
            const std::string s = entry.path().string();
            if (s.find("/.hub/") == std::string::npos &&
                s.find("/.git/") == std::string::npos) {
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Internal is_active helper
// ---------------------------------------------------------------------------

namespace {

bool is_active(std::string_view name) {
    const char* env = std::getenv("HERMES_HOME");
    if (env == nullptr || *env == '\0') return false;
    std::error_code ec;
    const auto active = fs::weakly_canonical(fs::path(env), ec);
    if (ec) return false;
    const auto target =
        fs::weakly_canonical(get_profiles_root() / std::string(name), ec);
    if (ec) return false;
    return active == target;
}

void copy_file_c2(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

}  // namespace

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

std::vector<ProfileInfo> list_profile_infos() {
    std::vector<ProfileInfo> out;
    const auto wrapper_dir = get_wrapper_dir();

    const auto default_home = get_default_hermes_home();
    std::error_code ec;
    if (fs::is_directory(default_home, ec)) {
        ProfileInfo info;
        info.name = "default";
        info.path = default_home;
        info.is_default = true;
        info.gateway_running = check_gateway_running(default_home);
        read_config_model(default_home, info.model, info.provider);
        info.has_env = fs::exists(default_home / ".env", ec);
        info.skill_count = count_skills(default_home);
        out.push_back(std::move(info));
    }

    const auto root = get_profiles_root();
    if (fs::is_directory(root, ec)) {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (entry.is_directory(ec) && !ec) {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& p : entries) {
            const std::string n = p.filename().string();
            if (!is_valid_profile_name(n)) continue;
            ProfileInfo info;
            info.name = n;
            info.path = p;
            info.is_default = false;
            info.gateway_running = check_gateway_running(p);
            read_config_model(p, info.model, info.provider);
            info.has_env = fs::exists(p / ".env", ec);
            info.skill_count = count_skills(p);
            const auto alias = wrapper_dir / n;
            if (fs::exists(alias, ec)) info.alias_path = alias;
            out.push_back(std::move(info));
        }
    }
    return out;
}

std::vector<std::string> list_profiles() {
    std::vector<std::string> out;
    const auto root = get_profiles_root();
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return out;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_directory(ec) && !ec) {
            out.push_back(entry.path().filename().string());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CRUD operations
// ---------------------------------------------------------------------------

fs::path create_profile_ex(std::string_view name, const CreateOptions& opts) {
    validate_profile_name(name);
    if (name == "default") {
        throw std::runtime_error(
            "Cannot create a profile named 'default' — it is the built-in "
            "profile (~/.hermes).");
    }
    const auto profile_dir = get_profile_dir(name);
    std::error_code ec;
    if (fs::exists(profile_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") + std::string(name) +
                                 "' already exists at " + profile_dir.string());
    }

    fs::path source_dir;
    const bool want_source =
        !opts.clone_from.empty() || opts.clone_all || opts.clone_config;
    if (want_source) {
        if (opts.clone_from.empty()) {
            source_dir = hermes::core::path::get_hermes_home();
        } else {
            validate_profile_name(opts.clone_from);
            source_dir = get_profile_dir(opts.clone_from);
        }
        if (!fs::is_directory(source_dir, ec)) {
            throw std::runtime_error(
                std::string("Source profile '") +
                (opts.clone_from.empty() ? std::string("active")
                                         : opts.clone_from) +
                "' does not exist at " + source_dir.string());
        }
    }

    if (opts.clone_all && !source_dir.empty()) {
        fs::copy(source_dir, profile_dir,
                 fs::copy_options::recursive |
                     fs::copy_options::copy_symlinks,
                 ec);
        if (ec) {
            throw std::runtime_error(
                "clone_all: copy failed: " + ec.message());
        }
        for (const auto& stale : clone_all_strip()) {
            fs::remove(profile_dir / stale, ec);
        }
    } else {
        fs::create_directories(profile_dir, ec);
        for (const auto& sub : profile_dirs()) {
            fs::create_directories(profile_dir / sub, ec);
        }
        if (!source_dir.empty()) {
            for (const auto& filename : clone_config_files()) {
                const auto src = source_dir / filename;
                if (fs::exists(src, ec)) {
                    copy_file_c2(src, profile_dir / filename);
                }
            }
            for (const auto& relpath : clone_subdir_files()) {
                const auto src = source_dir / relpath;
                if (fs::exists(src, ec)) {
                    copy_file_c2(src, profile_dir / relpath);
                }
            }
        } else {
            const auto cfg_src =
                hermes::core::path::get_default_hermes_root() / "config.yaml";
            const auto cfg_dst = profile_dir / "config.yaml";
            if (fs::exists(cfg_src, ec) && !fs::exists(cfg_dst, ec)) {
                auto maybe = hermes::core::atomic_io::atomic_read(cfg_src);
                if (maybe.has_value()) {
                    hermes::core::atomic_io::atomic_write(cfg_dst, *maybe);
                }
            } else if (!fs::exists(cfg_dst, ec)) {
                hermes::core::atomic_io::atomic_write(cfg_dst, std::string{});
            }
        }
    }
    return profile_dir;
}

void create_profile(std::string_view name) {
    if (name.empty()) {
        throw std::runtime_error(
            "hermes::profile::create_profile: empty profile name");
    }
    validate_profile_name(name);
    const auto target = get_profiles_root() / std::string(name);
    std::error_code ec;
    if (fs::exists(target / "config.yaml", ec)) return;
    fs::create_directories(target, ec);
    if (ec) {
        throw std::runtime_error(
            "hermes::profile::create_profile: mkdir failed for " +
            target.string() + ": " + ec.message());
    }
    const auto cfg_dst = target / "config.yaml";
    const auto cfg_src =
        hermes::core::path::get_default_hermes_root() / "config.yaml";
    if (fs::exists(cfg_src, ec) && !ec) {
        auto maybe = hermes::core::atomic_io::atomic_read(cfg_src);
        if (maybe.has_value()) {
            hermes::core::atomic_io::atomic_write(cfg_dst, *maybe);
            return;
        }
    }
    hermes::core::atomic_io::atomic_write(cfg_dst, std::string{});
}

fs::path delete_profile_ex(std::string_view name, bool yes) {
    validate_profile_name(name);
    if (name == "default") {
        throw std::runtime_error(
            "Cannot delete the default profile (~/.hermes).\n"
            "To remove everything, use: hermes uninstall");
    }
    if (is_active(name)) {
        throw std::runtime_error(
            std::string("hermes::profile::delete_profile: cannot delete the "
                        "currently-active profile: ") +
            std::string(name));
    }
    const auto profile_dir = get_profile_dir(name);
    std::error_code ec;
    if (!fs::is_directory(profile_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") + std::string(name) +
                                 "' does not exist.");
    }

    std::string model, provider;
    read_config_model(profile_dir, model, provider);
    const bool gw_running = check_gateway_running(profile_dir);
    const int skill_count = count_skills(profile_dir);

    std::cout << "\nProfile: " << std::string(name) << "\n"
              << "Path:    " << profile_dir.string() << "\n";
    if (!model.empty()) {
        std::cout << "Model:   " << model;
        if (!provider.empty()) std::cout << " (" << provider << ")";
        std::cout << "\n";
    }
    if (skill_count) std::cout << "Skills:  " << skill_count << "\n";

    std::vector<std::string> items = {
        "All config, API keys, memories, sessions, skills, cron jobs",
    };
    const auto wrapper_path = get_wrapper_dir() / std::string(name);
    const bool has_wrapper = fs::exists(wrapper_path, ec);
    if (has_wrapper) {
        items.push_back(std::string("Command alias (") + wrapper_path.string() +
                        ")");
    }
    std::cout << "\nThis will permanently delete:\n";
    for (const auto& it : items) std::cout << "  - " << it << "\n";
    if (gw_running) {
        std::cout << "  ! Gateway is running - it will be stopped.\n";
    }

    if (!yes) {
        std::cout << "\nType '" << std::string(name) << "' to confirm: "
                  << std::flush;
        std::string confirm;
        if (!std::getline(std::cin, confirm)) {
            std::cout << "\nCancelled.\n";
            return profile_dir;
        }
        while (!confirm.empty() &&
               (confirm.back() == '\n' || confirm.back() == '\r' ||
                confirm.back() == ' ' || confirm.back() == '\t'))
            confirm.pop_back();
        if (confirm != std::string(name)) {
            std::cout << "Cancelled.\n";
            return profile_dir;
        }
    }

    cleanup_gateway_service(name, profile_dir);
    if (gw_running) stop_gateway_process(profile_dir);

    if (has_wrapper) {
        if (remove_wrapper_script(name)) {
            std::cout << "Removed " << wrapper_path.string() << "\n";
        }
    }

    fs::remove_all(profile_dir, ec);
    if (ec) {
        std::cerr << "warn: could not remove " << profile_dir << ": "
                  << ec.message() << "\n";
    } else {
        std::cout << "Removed " << profile_dir.string() << "\n";
    }

    try {
        if (get_active_profile() == std::string(name)) {
            set_active_profile("default");
            std::cout << "Active profile reset to default\n";
        }
    } catch (...) {
    }

    std::cout << "\nProfile '" << std::string(name) << "' deleted.\n";
    return profile_dir;
}

void delete_profile(std::string_view name) {
    if (name.empty()) {
        throw std::runtime_error(
            "hermes::profile::delete_profile: empty profile name");
    }
    if (is_active(name)) {
        throw std::runtime_error(
            std::string("hermes::profile::delete_profile: cannot delete "
                        "the currently-active profile: ") +
            std::string(name));
    }
    const auto target = get_profiles_root() / std::string(name);
    std::error_code ec;
    if (!fs::exists(target, ec)) return;
    fs::remove_all(target, ec);
    if (ec) {
        throw std::runtime_error(
            "hermes::profile::delete_profile: remove_all failed for " +
            target.string() + ": " + ec.message());
    }
}

fs::path rename_profile(std::string_view old_name, std::string_view new_name) {
    validate_profile_name(old_name);
    validate_profile_name(new_name);
    if (old_name == "default") {
        throw std::runtime_error("Cannot rename the default profile.");
    }
    if (new_name == "default") {
        throw std::runtime_error(
            "Cannot rename to 'default' — it is reserved.");
    }
    if (is_active(old_name)) {
        throw std::runtime_error(
            std::string("hermes::profile::rename_profile: cannot rename the "
                        "currently-active profile: ") +
            std::string(old_name));
    }
    const auto old_dir = get_profile_dir(old_name);
    const auto new_dir = get_profile_dir(new_name);
    std::error_code ec;
    if (!fs::is_directory(old_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") +
                                 std::string(old_name) + "' does not exist.");
    }
    if (fs::exists(new_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") +
                                 std::string(new_name) + "' already exists.");
    }

    if (check_gateway_running(old_dir)) {
        cleanup_gateway_service(old_name, old_dir);
        stop_gateway_process(old_dir);
    }

    fs::rename(old_dir, new_dir, ec);
    if (ec) {
        throw std::runtime_error("rename failed: " + ec.message());
    }
    std::cout << "Renamed " << old_dir.filename().string() << " -> "
              << new_dir.filename().string() << "\n";

    remove_wrapper_script(old_name);
    const std::string collision = check_alias_collision(new_name);
    if (collision.empty()) {
        if (!create_wrapper_script(new_name).empty()) {
            std::cout << "Alias updated: " << std::string(new_name) << "\n";
        }
    } else {
        std::cout << "warn: cannot create alias '" << std::string(new_name)
                  << "' - " << collision << "\n";
    }

    try {
        if (get_active_profile() == std::string(old_name)) {
            set_active_profile(new_name);
            std::cout << "Active profile updated: " << std::string(new_name)
                      << "\n";
        }
    } catch (...) {
    }
    return new_dir;
}

// ---------------------------------------------------------------------------
// Active profile (sticky)
// ---------------------------------------------------------------------------

std::string get_active_profile() {
    const auto path = get_active_profile_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) return "default";
    std::string body = read_file_safe(path);
    while (!body.empty() &&
           (body.back() == '\n' || body.back() == '\r' ||
            body.back() == ' ' || body.back() == '\t'))
        body.pop_back();
    if (body.empty()) return "default";
    return body;
}

void set_active_profile(std::string_view name) {
    validate_profile_name(name);
    if (name != "default" && !profile_exists(name)) {
        throw std::runtime_error(
            std::string("Profile '") + std::string(name) +
            "' does not exist. Create it with: hermes profile create " +
            std::string(name));
    }
    const auto path = get_active_profile_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (name == "default") {
        fs::remove(path, ec);
        return;
    }
    hermes::core::atomic_io::atomic_write(path, std::string(name) + "\n");
}

std::string get_active_profile_name() {
    const auto hermes_home = hermes::core::path::get_hermes_home();
    std::error_code ec;
    const auto resolved = fs::weakly_canonical(hermes_home, ec);
    const auto default_resolved =
        fs::weakly_canonical(get_default_hermes_home(), ec);
    if (!ec && resolved == default_resolved) return "default";

    const auto profiles_root = fs::weakly_canonical(get_profiles_root(), ec);
    if (ec) return "custom";

    auto it_a = profiles_root.begin();
    auto it_b = resolved.begin();
    while (it_a != profiles_root.end() && it_b != resolved.end() &&
           *it_a == *it_b) {
        ++it_a;
        ++it_b;
    }
    if (it_a != profiles_root.end()) return "custom";
    if (it_b == resolved.end()) return "custom";
    const std::string name = it_b->string();
    ++it_b;
    if (it_b != resolved.end()) return "custom";
    if (!is_valid_profile_name(name)) return "custom";
    return name;
}

// ---------------------------------------------------------------------------
// Export / Import
// ---------------------------------------------------------------------------

bool default_export_ignored(const fs::path& root_dir,
                            const fs::path& directory,
                            std::string_view entry) {
    if (entry == "__pycache__") return true;
    if (entry.size() >= 5 && entry.substr(entry.size() - 5) == ".sock")
        return true;
    if (entry.size() >= 4 && entry.substr(entry.size() - 4) == ".tmp")
        return true;
    if (entry == "package.json" || entry == "package-lock.json") return true;
    std::error_code ec;
    if (fs::equivalent(directory, root_dir, ec)) {
        if (is_default_export_excluded_root(entry)) return true;
    }
    return false;
}

std::vector<std::string> normalize_profile_archive_parts(
    std::string_view member_name) {
    if (member_name.empty()) {
        throw std::runtime_error(std::string("Unsafe archive member path: ") +
                                 std::string(member_name));
    }
    std::string normalized(member_name);
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    if (!normalized.empty() && normalized.front() == '/') {
        throw std::runtime_error(std::string("Unsafe archive member path: ") +
                                 std::string(member_name));
    }
    if (normalized.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(normalized[0])) &&
        normalized[1] == ':') {
        throw std::runtime_error(std::string("Unsafe archive member path: ") +
                                 std::string(member_name));
    }
    std::vector<std::string> parts;
    std::string cur;
    for (char c : normalized) {
        if (c == '/') {
            if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur != ".") parts.push_back(cur);
    if (parts.empty()) {
        throw std::runtime_error(std::string("Unsafe archive member path: ") +
                                 std::string(member_name));
    }
    for (const auto& p : parts) {
        if (p == "..") {
            throw std::runtime_error(
                std::string("Unsafe archive member path: ") +
                std::string(member_name));
        }
    }
    return parts;
}

namespace {

std::string shell_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

int run_tar_czf(const fs::path& archive, const fs::path& parent,
                const std::string& dirname,
                const std::vector<std::string>& excludes) {
    std::string cmd = "tar -czf " + shell_escape(archive.string()) +
                      " -C " + shell_escape(parent.string());
    for (const auto& ex : excludes) {
        cmd += " --exclude=" + shell_escape(ex);
    }
    cmd += " " + shell_escape(dirname);
    return std::system(cmd.c_str());
}

int run_tar_xzf(const fs::path& archive, const fs::path& destination) {
    std::string cmd = "tar -xzf " + shell_escape(archive.string()) + " -C " +
                      shell_escape(destination.string());
    return std::system(cmd.c_str());
}

std::set<std::string> tar_list_top_dirs(const fs::path& archive) {
    std::set<std::string> out;
    std::string cmd = "tar -tzf " + shell_escape(archive.string());
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        try {
            auto parts = normalize_profile_archive_parts(line);
            if (!parts.empty()) out.insert(parts[0]);
        } catch (...) {
        }
    }
    ::pclose(pipe);
    return out;
}

}  // namespace

fs::path export_profile(std::string_view name, const fs::path& output_path) {
    validate_profile_name(name);
    const auto profile_dir = get_profile_dir(name);
    std::error_code ec;
    if (!fs::is_directory(profile_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") + std::string(name) +
                                 "' does not exist.");
    }

    fs::path output = output_path;
    std::string out_str = output.string();
    if (out_str.size() < 7 || out_str.substr(out_str.size() - 7) != ".tar.gz") {
        if (out_str.size() >= 4 && out_str.substr(out_str.size() - 4) == ".tgz") {
            out_str = out_str.substr(0, out_str.size() - 4) + ".tar.gz";
        } else {
            out_str += ".tar.gz";
        }
        output = out_str;
    }

    std::vector<std::string> excludes;
    excludes.push_back("__pycache__");
    excludes.push_back("*.sock");
    excludes.push_back("*.tmp");

    if (name == "default") {
        static const std::array<const char*, 25> root_excludes = {
            "hermes-agent", ".worktrees", "profiles", "bin", "node_modules",
            "state.db", "state.db-shm", "state.db-wal",
            "hermes_state.db",
            "response_store.db", "response_store.db-shm", "response_store.db-wal",
            "gateway.pid", "gateway_state.json", "processes.json",
            "auth.json", ".env", "auth.lock", "active_profile", ".update_check",
            "errors.log", ".hermes_history",
            "image_cache", "audio_cache", "document_cache",
        };
        for (const auto* r : root_excludes) {
            excludes.push_back(std::string("default/") + r);
        }
        const auto tmpdir = fs::temp_directory_path() /
                            ("hermes-export-" + std::to_string(::getpid()));
        fs::remove_all(tmpdir, ec);
        fs::create_directories(tmpdir, ec);
        const auto staged = tmpdir / "default";
        fs::copy(profile_dir, staged,
                 fs::copy_options::recursive |
                     fs::copy_options::copy_symlinks,
                 ec);
        int rc = run_tar_czf(output, tmpdir, "default", excludes);
        fs::remove_all(tmpdir, ec);
        if (rc != 0) {
            throw std::runtime_error("tar exited with code " +
                                     std::to_string(rc));
        }
    } else {
        const auto tmpdir = fs::temp_directory_path() /
                            ("hermes-export-" + std::to_string(::getpid()));
        fs::remove_all(tmpdir, ec);
        fs::create_directories(tmpdir, ec);
        const auto staged = tmpdir / std::string(name);
        fs::copy(profile_dir, staged,
                 fs::copy_options::recursive |
                     fs::copy_options::copy_symlinks,
                 ec);
        fs::remove(staged / "auth.json", ec);
        fs::remove(staged / ".env", ec);
        int rc = run_tar_czf(output, tmpdir, std::string(name), excludes);
        fs::remove_all(tmpdir, ec);
        if (rc != 0) {
            throw std::runtime_error("tar exited with code " +
                                     std::to_string(rc));
        }
    }
    return output;
}

fs::path import_profile(const fs::path& archive_path, std::string_view name) {
    std::error_code ec;
    if (!fs::exists(archive_path, ec)) {
        throw std::runtime_error("Archive not found: " + archive_path.string());
    }

    std::set<std::string> top_dirs = tar_list_top_dirs(archive_path);
    std::string inferred_name =
        name.empty() ? (top_dirs.size() == 1 ? *top_dirs.begin() : std::string())
                     : std::string(name);

    if (inferred_name.empty()) {
        throw std::runtime_error(
            "Cannot determine profile name from archive. Specify with "
            "--name <name>.");
    }
    if (inferred_name == "default") {
        throw std::runtime_error(
            "Cannot import as 'default' — that is the built-in root profile. "
            "Specify a different name with --name <name>.");
    }

    validate_profile_name(inferred_name);
    const auto profile_dir = get_profile_dir(inferred_name);
    if (fs::exists(profile_dir, ec)) {
        throw std::runtime_error(std::string("Profile '") + inferred_name +
                                 "' already exists at " + profile_dir.string());
    }

    const auto profiles_root = get_profiles_root();
    fs::create_directories(profiles_root, ec);

    int rc = run_tar_xzf(archive_path, profiles_root);
    if (rc != 0) {
        throw std::runtime_error("tar -xzf exited with code " +
                                 std::to_string(rc));
    }

    if (!top_dirs.empty()) {
        const std::string original_top = *top_dirs.begin();
        const auto extracted = profiles_root / original_top;
        if (extracted != profile_dir && fs::exists(extracted, ec)) {
            fs::rename(extracted, profile_dir, ec);
        }
    }
    return profile_dir;
}

// ---------------------------------------------------------------------------
// Gateway helpers
// ---------------------------------------------------------------------------

void cleanup_gateway_service(std::string_view name,
                             const fs::path& /*profile_dir*/) {
#if defined(__linux__)
    const std::string svc_name = std::string("hermes-gateway-") +
                                 std::string(name) + ".service";
    const char* home_env = std::getenv("HOME");
    if (!home_env) return;
    fs::path svc_file = fs::path(home_env) / ".config" / "systemd" / "user" /
                        svc_name;
    std::error_code ec;
    if (fs::exists(svc_file, ec)) {
        std::string cmd1 = "systemctl --user disable " + svc_name +
                           " >/dev/null 2>&1";
        (void)!std::system(cmd1.c_str());
        std::string cmd2 = "systemctl --user stop " + svc_name +
                           " >/dev/null 2>&1";
        (void)!std::system(cmd2.c_str());
        fs::remove(svc_file, ec);
        (void)!std::system("systemctl --user daemon-reload >/dev/null 2>&1");
        std::cout << "Service " << svc_name << " removed\n";
    }
#elif defined(__APPLE__)
    const char* home_env = std::getenv("HOME");
    if (!home_env) return;
    fs::path plist_path = fs::path(home_env) / "Library" / "LaunchAgents" /
                          (std::string("com.hermes.gateway.") +
                           std::string(name) + ".plist");
    std::error_code ec;
    if (fs::exists(plist_path, ec)) {
        std::string cmd = "launchctl unload " + plist_path.string() +
                          " >/dev/null 2>&1";
        (void)!std::system(cmd.c_str());
        fs::remove(plist_path, ec);
        std::cout << "Launchd service removed\n";
    }
#else
    (void)name;
#endif
}

void stop_gateway_process(const fs::path& profile_dir) {
    const auto pid_file = profile_dir / "gateway.pid";
    std::error_code ec;
    if (!fs::exists(pid_file, ec)) return;
    std::string raw = read_file_safe(pid_file);
    while (!raw.empty() &&
           (raw.back() == '\n' || raw.back() == '\r' ||
            raw.back() == ' ' || raw.back() == '\t'))
        raw.pop_back();
    if (raw.empty()) return;
    long pid = 0;
    try {
        if (raw.front() == '{') {
            auto p = raw.find("\"pid\"");
            if (p == std::string::npos) return;
            p = raw.find(':', p);
            if (p == std::string::npos) return;
            ++p;
            while (p < raw.size() &&
                   std::isspace(static_cast<unsigned char>(raw[p])))
                ++p;
            size_t end = p;
            while (end < raw.size() &&
                   std::isdigit(static_cast<unsigned char>(raw[end])))
                ++end;
            pid = std::stol(raw.substr(p, end - p));
        } else {
            pid = std::stol(raw);
        }
    } catch (...) {
        return;
    }
    if (pid <= 0) return;
#if defined(__unix__) || defined(__APPLE__)
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        if (errno == ESRCH) {
            std::cout << "Gateway already stopped\n";
            return;
        }
    }
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            std::cout << "Gateway stopped (PID " << pid << ")\n";
            return;
        }
    }
    ::kill(static_cast<pid_t>(pid), SIGKILL);
    std::cout << "Gateway force-stopped (PID " << pid << ")\n";
#else
    (void)pid;
#endif
}

// ---------------------------------------------------------------------------
// argv preparse (-p/--profile)
// ---------------------------------------------------------------------------

std::optional<std::string> preparse_profile_argv(int& argc, char* argv[]) {
    if (argc < 2 || argv == nullptr) return std::nullopt;

    for (int i = 1; i < argc; ++i) {
        const std::string_view tok = argv[i];
        if (tok == "--") break;

        static constexpr std::string_view kEqPrefix = "--profile=";
        if (tok.size() > kEqPrefix.size() &&
            tok.substr(0, kEqPrefix.size()) == kEqPrefix) {
            std::string name(tok.substr(kEqPrefix.size()));
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc;
            argv[argc] = nullptr;
            if (name.empty()) return std::nullopt;
            return name;
        }

        const bool is_long = (tok == "--profile");
        const bool is_short = (tok == "-p");
        if (is_long || is_short) {
            if (i + 1 >= argc) return std::nullopt;
            std::string name(argv[i + 1]);
            if (name.empty()) return std::nullopt;
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            argv[argc] = nullptr;
            if (argc + 1 >= 0) argv[argc + 1] = nullptr;
            return name;
        }
    }
    return std::nullopt;
}

void apply_profile_override(std::optional<std::string> profile_name) {
    if (!profile_name.has_value() || profile_name->empty()) return;
    const auto dir = get_profiles_root() / *profile_name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    ::setenv("HERMES_HOME", dir.c_str(), 1);
}

std::string resolve_profile_env(std::string_view profile_name) {
    validate_profile_name(profile_name);
    const auto profile_dir = get_profile_dir(profile_name);
    std::error_code ec;
    if (profile_name != "default" && !fs::is_directory(profile_dir, ec)) {
        throw std::runtime_error(
            std::string("Profile '") + std::string(profile_name) +
            "' does not exist. Create it with: hermes profile create " +
            std::string(profile_name));
    }
    return profile_dir.string();
}

// ---------------------------------------------------------------------------
// Tab completion
// ---------------------------------------------------------------------------

std::string generate_bash_completion() {
    return R"COMPL(# Hermes Agent profile completion
# Add to ~/.bashrc: eval "$(hermes completion bash)"

_hermes_profiles() {
    local profiles_dir="$HOME/.hermes/profiles"
    local profiles="default"
    if [ -d "$profiles_dir" ]; then
        profiles="$profiles $(ls "$profiles_dir" 2>/dev/null)"
    fi
    echo "$profiles"
}

_hermes_completion() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    if [[ "$prev" == "-p" || "$prev" == "--profile" ]]; then
        COMPREPLY=($(compgen -W "$(_hermes_profiles)" -- "$cur"))
        return
    fi

    if [[ "${COMP_WORDS[1]}" == "profile" ]]; then
        case "$prev" in
            profile)
                COMPREPLY=($(compgen -W "list use create delete show alias rename export import" -- "$cur"))
                return
                ;;
            use|delete|show|alias|rename|export)
                COMPREPLY=($(compgen -W "$(_hermes_profiles)" -- "$cur"))
                return
                ;;
        esac
    fi

    if [[ "$COMP_CWORD" == 1 ]]; then
        local commands="chat model gateway setup status cron doctor dump config skills tools mcp sessions profile update version"
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
    fi
}

complete -F _hermes_completion hermes
)COMPL";
}

std::string generate_zsh_completion() {
    return R"COMPL(#compdef hermes
# Hermes Agent profile completion
# Add to ~/.zshrc: eval "$(hermes completion zsh)"

_hermes() {
    local -a profiles
    profiles=(default)
    if [[ -d "$HOME/.hermes/profiles" ]]; then
        profiles+=("${(@f)$(ls $HOME/.hermes/profiles 2>/dev/null)}")
    fi

    _arguments \
        '-p[Profile name]:profile:($profiles)' \
        '--profile[Profile name]:profile:($profiles)' \
        '1:command:(chat model gateway setup status cron doctor dump config skills tools mcp sessions profile update version)' \
        '*::arg:->args'

    case $words[1] in
        profile)
            _arguments '1:action:(list use create delete show alias rename export import)' \
                        '2:profile:($profiles)'
            ;;
    esac
}

_hermes "$@"
)COMPL";
}

// ---------------------------------------------------------------------------
// Interactive wizard + display
// ---------------------------------------------------------------------------

namespace {

std::string prompt(const std::string& msg, const std::string& def = "") {
    std::cout << msg;
    if (!def.empty()) std::cout << " [" << def << "]";
    std::cout << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' ||
            line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (line.empty()) return def;
    return line;
}

bool prompt_yes(const std::string& msg, bool def = false) {
    std::cout << msg << " [" << (def ? "Y/n" : "y/N") << "]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' ||
            line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (line.empty()) return def;
    return line[0] == 'y' || line[0] == 'Y';
}

}  // namespace

int wizard_create(std::string_view name) {
    try {
        validate_profile_name(name);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    if (name == "default") {
        std::cerr << "Error: 'default' is reserved.\n";
        return 1;
    }
    std::error_code ec;
    if (fs::exists(get_profile_dir(name), ec)) {
        std::cerr << "Error: Profile '" << std::string(name)
                  << "' already exists.\n";
        return 1;
    }

    std::cout << "\nCreating profile: " << std::string(name) << "\n";
    std::cout << "Leave fields blank to accept defaults.\n\n";

    std::string clone_source = prompt(
        "Clone settings from an existing profile (leave blank for none)", "");
    CreateOptions opts;
    opts.no_alias = !prompt_yes("Install wrapper alias for '" +
                                    std::string(name) + "'?",
                                true);
    if (!clone_source.empty()) {
        opts.clone_from = clone_source;
        opts.clone_config = prompt_yes("Clone config files only?", true);
    }

    try {
        const auto dir = create_profile_ex(name, opts);
        std::cout << "\nCreated " << dir.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    const std::string model = prompt("Default model", "");
    const std::string provider = prompt("Default provider", "");
    if (!model.empty() || !provider.empty()) {
        const auto cfg_path = get_profile_dir(name) / "config.yaml";
        std::string body;
        if (!model.empty()) body += "model:\n  default: " + model + "\n";
        if (!provider.empty()) {
            if (body.empty()) body += "model:\n";
            body += "  provider: " + provider + "\n";
        }
        if (fs::exists(cfg_path, ec)) {
            std::string existing = read_file_safe(cfg_path);
            if (!existing.empty() && existing.back() != '\n')
                existing.push_back('\n');
            body = existing + body;
        }
        hermes::core::atomic_io::atomic_write(cfg_path, body);
        std::cout << "Wrote model configuration to " << cfg_path.string()
                  << "\n";
    }

    const std::string cwd = prompt("Default working directory", "");
    if (!cwd.empty()) {
        const auto env_path = get_profile_dir(name) / ".env";
        std::string existing = read_file_safe(env_path);
        if (!existing.empty() && existing.back() != '\n')
            existing.push_back('\n');
        existing += "MESSAGING_CWD=" + cwd + "\n";
        hermes::core::atomic_io::atomic_write(env_path, existing);
        std::cout << "Set MESSAGING_CWD in " << env_path.string() << "\n";
    }

    if (prompt_yes("Add Telegram bot token now?", false)) {
        const std::string tok = prompt("TELEGRAM_BOT_TOKEN", "");
        if (!tok.empty()) {
            const auto env_path = get_profile_dir(name) / ".env";
            std::string existing = read_file_safe(env_path);
            if (!existing.empty() && existing.back() != '\n')
                existing.push_back('\n');
            existing += "TELEGRAM_BOT_TOKEN=" + tok + "\n";
            hermes::core::atomic_io::atomic_write(env_path, existing);
        }
    }

    if (!opts.no_alias) {
        const std::string collision = check_alias_collision(name);
        if (collision.empty()) {
            const auto path = create_wrapper_script(name);
            if (!path.empty()) {
                std::cout << "Alias: " << path.string() << "\n";
                if (!is_wrapper_dir_in_path()) {
                    std::cout
                        << "warn: " << get_wrapper_dir().string()
                        << " is not in $PATH; add it so the alias works.\n";
                }
            }
        } else {
            std::cout << "warn: skipping alias - " << collision << "\n";
        }
    }

    std::cout << "\nDone. Launch with: hermes -p " << std::string(name)
              << "\n";
    return 0;
}

int print_profile_table(const std::vector<ProfileInfo>& infos) {
    if (infos.empty()) {
        std::cout << "No profiles.\n";
        return 0;
    }
    size_t w_name = 4, w_model = 5, w_provider = 8;
    for (const auto& i : infos) {
        w_name = std::max(w_name, i.name.size());
        w_model = std::max(w_model, i.model.size());
        w_provider = std::max(w_provider, i.provider.size());
    }

    auto pad = [](const std::string& s, size_t n) {
        if (s.size() >= n) return s;
        return s + std::string(n - s.size(), ' ');
    };

    std::cout << pad("NAME", w_name) << "  " << pad("MODEL", w_model) << "  "
              << pad("PROVIDER", w_provider) << "  SKILLS  ENV  GATEWAY\n";
    for (const auto& i : infos) {
        std::cout << pad(i.name, w_name) << "  "
                  << pad(i.model.empty() ? "-" : i.model, w_model) << "  "
                  << pad(i.provider.empty() ? "-" : i.provider, w_provider)
                  << "  " << pad(std::to_string(i.skill_count), 6) << "  "
                  << (i.has_env ? "yes" : "no ") << "  "
                  << (i.gateway_running ? "running" : "stopped") << "\n";
    }
    return 0;
}

int print_show(std::string_view name) {
    std::error_code ec;
    const auto dir = get_profile_dir(name);
    if (!fs::exists(dir, ec)) {
        std::cerr << "Profile '" << std::string(name) << "' does not exist.\n";
        return 1;
    }
    ProfileInfo info;
    info.name = name;
    info.path = dir;
    info.is_default = (name == "default");
    info.gateway_running = check_gateway_running(dir);
    read_config_model(dir, info.model, info.provider);
    info.has_env = fs::exists(dir / ".env", ec);
    info.skill_count = count_skills(dir);
    const auto alias = get_wrapper_dir() / std::string(name);
    if (fs::exists(alias, ec)) info.alias_path = alias;

    std::cout << "Profile: " << info.name << "\n"
              << "Path:    " << info.path.string() << "\n";
    if (!info.model.empty()) std::cout << "Model:   " << info.model << "\n";
    if (!info.provider.empty())
        std::cout << "Provider:" << info.provider << "\n";
    std::cout << "Env:     " << (info.has_env ? "yes" : "no") << "\n"
              << "Skills:  " << info.skill_count << "\n"
              << "Gateway: " << (info.gateway_running ? "running" : "stopped")
              << "\n";
    if (!info.alias_path.empty()) {
        std::cout << "Alias:   " << info.alias_path.string() << "\n";
    }
    return 0;
}

}  // namespace hermes::profile
