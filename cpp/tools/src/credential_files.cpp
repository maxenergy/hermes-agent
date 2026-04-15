#include "hermes/tools/credential_files.hpp"

#include "hermes/core/logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>

namespace hermes::tools {

namespace {

namespace fs = std::filesystem;

std::filesystem::path home_or_cwd() {
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h);
    }
    return std::filesystem::current_path();
}

std::string strip_trailing_slash(std::string_view s) {
    std::string out(s);
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    return out;
}

// Process-global registry (skill-declared) plus the lock that guards it.
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::string>& registry_map() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}

// Cached temp dir for sanitized skills copy — recreated on each call so
// stale copies do not accumulate (mirrors the Python implementation).
fs::path& cached_safe_skills_dir() {
    static fs::path p;
    return p;
}

bool path_starts_with(const fs::path& candidate, const fs::path& root) {
    auto cs = candidate.string();
    auto rs = root.string();
    if (cs.size() < rs.size()) return false;
    return cs.compare(0, rs.size(), rs) == 0;
}

bool is_path_traversal(std::string_view rel) {
    // Reject obvious ../ traversal markers — even before resolve() the
    // common case is fast-filtered here.
    if (rel.find("..") == std::string_view::npos) return false;
    // Split on / and \\ and check for explicit ``..`` components.
    std::string buf;
    buf.reserve(rel.size());
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (buf == "..") return true;
            buf.clear();
        } else {
            buf.push_back(c);
        }
    }
    return buf == "..";
}

// Cache directory layout — (new_subpath, legacy_name) tuples.
const std::vector<std::pair<std::string, std::string>>& cache_dirs() {
    static const std::vector<std::pair<std::string, std::string>> v = {
        {"cache/documents", "document_cache"},
        {"cache/images", "image_cache"},
        {"cache/audio", "audio_cache"},
        {"cache/screenshots", "browser_screenshots"},
    };
    return v;
}

fs::path get_hermes_dir(const std::string& new_sub, const std::string& legacy) {
    auto root = hermes_home();
    auto preferred = root / new_sub;
    std::error_code ec;
    if (fs::is_directory(preferred, ec)) return preferred;
    auto legacy_path = root / legacy;
    if (fs::is_directory(legacy_path, ec)) return legacy_path;
    return preferred;
}

}  // namespace

// ---- Path helpers --------------------------------------------------------

fs::path hermes_home() {
    if (const char* override_home = std::getenv("HERMES_HOME");
        override_home && *override_home) {
        return fs::path(override_home);
    }
    return home_or_cwd() / ".hermes";
}

fs::path hermes_env_file() { return hermes_home() / ".env"; }

fs::path hermes_credentials_dir() { return hermes_home() / ".credentials"; }

fs::path credential_path(std::string_view relative) {
    return hermes_credentials_dir() / fs::path(std::string(relative));
}

std::vector<fs::path> list_credential_files() {
    std::vector<fs::path> out;
    std::error_code ec;
    const auto root = hermes_credentials_dir();
    if (!fs::exists(root, ec) || ec) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) {
            out.push_back(it->path());
        }
    }
    return out;
}

// ---- Containment ---------------------------------------------------------

std::optional<fs::path> resolve_contained_path(std::string_view relative) {
    if (relative.empty()) return std::nullopt;
    fs::path rel(std::string{relative});
    if (rel.is_absolute()) {
        hermes::core::logging::log_warn("credential_files: rejected absolute path " +
                        std::string(relative));
        return std::nullopt;
    }
    if (is_path_traversal(relative)) {
        hermes::core::logging::log_warn("credential_files: rejected traversal " +
                        std::string(relative));
        return std::nullopt;
    }

    auto root = hermes_home();
    auto host = root / rel;
    std::error_code ec;
    auto resolved = fs::weakly_canonical(host, ec);
    if (ec) resolved = host;
    auto root_canonical = fs::weakly_canonical(root, ec);
    if (ec) root_canonical = root;

    if (!path_starts_with(resolved, root_canonical)) {
        hermes::core::logging::log_warn(
            "credential_files: rejected path traversal (escapes HERMES_HOME)");
        return std::nullopt;
    }
    if (!fs::is_regular_file(resolved, ec)) return std::nullopt;
    return resolved;
}

// ---- Registry ------------------------------------------------------------

bool register_credential_file(std::string_view relative_path,
                              std::string_view container_base) {
    auto resolved = resolve_contained_path(relative_path);
    if (!resolved) return false;

    std::string key = strip_trailing_slash(container_base) + "/" +
                      std::string(relative_path);
    {
        std::lock_guard<std::mutex> lock(registry_mutex());
        registry_map()[key] = resolved->string();
    }
    return true;
}

std::vector<std::string> register_credential_files(
    const nlohmann::json& entries, std::string_view container_base) {
    std::vector<std::string> missing;
    if (!entries.is_array()) return missing;
    for (const auto& entry : entries) {
        std::string rel;
        if (entry.is_string()) {
            rel = entry.get<std::string>();
        } else if (entry.is_object()) {
            if (entry.contains("path") && entry["path"].is_string()) {
                rel = entry["path"].get<std::string>();
            } else if (entry.contains("name") && entry["name"].is_string()) {
                rel = entry["name"].get<std::string>();
            }
        } else {
            continue;
        }

        // Trim whitespace.
        while (!rel.empty() && std::isspace(static_cast<unsigned char>(rel.front()))) rel.erase(0, 1);
        while (!rel.empty() && std::isspace(static_cast<unsigned char>(rel.back()))) rel.pop_back();
        if (rel.empty()) continue;

        if (!register_credential_file(rel, container_base)) {
            missing.push_back(rel);
        }
    }
    return missing;
}

void clear_credential_files() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry_map().clear();
}

std::size_t registered_credential_count() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return registry_map().size();
}

std::vector<CredentialMount> get_credential_file_mounts() {
    std::vector<CredentialMount> out;
    std::lock_guard<std::mutex> lock(registry_mutex());
    for (const auto& [container_path, host_path] : registry_map()) {
        std::error_code ec;
        if (fs::is_regular_file(host_path, ec)) {
            out.push_back({host_path, container_path});
        }
    }
    return out;
}

// ---- Skills --------------------------------------------------------------

bool directory_contains_symlinks(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink()) return true;
    }
    return false;
}

fs::path safe_skills_path(const fs::path& skills_dir) {
    if (!directory_contains_symlinks(skills_dir)) return skills_dir;

    // Rebuild a sanitized copy.
    auto& cached = cached_safe_skills_dir();
    std::error_code ec;
    if (!cached.empty() && fs::is_directory(cached, ec)) {
        fs::remove_all(cached, ec);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::ostringstream name;
    name << "hermes-skills-safe-" << std::hex << gen();
    auto temp = fs::temp_directory_path() / name.str();
    fs::create_directories(temp, ec);
    cached = temp;

    for (auto it = fs::recursive_directory_iterator(skills_dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink()) continue;
        auto rel = fs::relative(it->path(), skills_dir, ec);
        auto dest = temp / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(dest, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(it->path(), dest,
                          fs::copy_options::overwrite_existing, ec);
        }
    }
    return temp;
}

std::vector<CredentialMount> get_skills_directory_mount(
    std::string_view container_base) {
    std::vector<CredentialMount> mounts;
    auto root = hermes_home();
    auto skills = root / "skills";
    std::error_code ec;
    if (fs::is_directory(skills, ec)) {
        auto host = safe_skills_path(skills);
        mounts.push_back(
            {host.string(), strip_trailing_slash(container_base) + "/skills"});
    }
    return mounts;
}

std::vector<CredentialMount> iter_skills_files(
    std::string_view container_base) {
    std::vector<CredentialMount> out;
    auto root = hermes_home() / "skills";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    auto base = strip_trailing_slash(container_base) + "/skills";
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink()) continue;
        if (!it->is_regular_file(ec)) continue;
        auto rel = fs::relative(it->path(), root, ec);
        out.push_back({it->path().string(), base + "/" + rel.generic_string()});
    }
    return out;
}

// ---- Cache ---------------------------------------------------------------

std::vector<CredentialMount> get_cache_directory_mounts(
    std::string_view container_base) {
    std::vector<CredentialMount> mounts;
    auto base = strip_trailing_slash(container_base);
    for (const auto& [new_sub, legacy] : cache_dirs()) {
        auto host = get_hermes_dir(new_sub, legacy);
        std::error_code ec;
        if (fs::is_directory(host, ec)) {
            mounts.push_back({host.string(), base + "/" + new_sub});
        }
    }
    return mounts;
}

std::vector<CredentialMount> iter_cache_files(std::string_view container_base) {
    std::vector<CredentialMount> out;
    auto base = strip_trailing_slash(container_base);
    for (const auto& [new_sub, legacy] : cache_dirs()) {
        auto host = get_hermes_dir(new_sub, legacy);
        std::error_code ec;
        if (!fs::is_directory(host, ec)) continue;
        auto container_root = base + "/" + new_sub;
        for (auto it = fs::recursive_directory_iterator(host, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_symlink()) continue;
            if (!it->is_regular_file(ec)) continue;
            auto rel = fs::relative(it->path(), host, ec);
            out.push_back(
                {it->path().string(), container_root + "/" + rel.generic_string()});
        }
    }
    return out;
}

}  // namespace hermes::tools
