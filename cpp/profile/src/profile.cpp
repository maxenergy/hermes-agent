#include "hermes/profile/profile.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace hermes::profile {

namespace fs = std::filesystem;

namespace {

// True when HERMES_HOME points at `<profiles_root>/<name>`.
bool is_active(std::string_view name) {
    const char* env = std::getenv("HERMES_HOME");
    if (env == nullptr || *env == '\0') {
        return false;
    }
    std::error_code ec;
    const auto active = fs::weakly_canonical(fs::path(env), ec);
    if (ec) {
        return false;
    }
    const auto target = fs::weakly_canonical(
        get_profiles_root() / std::string(name), ec);
    if (ec) {
        return false;
    }
    return active == target;
}

void require_nonempty(std::string_view name, const char* context) {
    if (name.empty()) {
        throw std::runtime_error(
            std::string("hermes::profile: empty profile name for ") + context);
    }
}

}  // namespace

fs::path get_profiles_root() {
    // Proxies to the core helper which is *deliberately* HOME-anchored
    // — see header invariant.
    return hermes::core::path::get_profiles_root();
}

void apply_profile_override(std::optional<std::string> profile_name) {
    if (!profile_name.has_value() || profile_name->empty()) {
        return;
    }
    const auto dir = get_profiles_root() / *profile_name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    // setenv(3) is POSIX; Windows support lands with Phase 13.
    ::setenv("HERMES_HOME", dir.c_str(), 1);
}

std::vector<std::string> list_profiles() {
    std::vector<std::string> out;
    const auto root = get_profiles_root();
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_directory(ec) && !ec) {
            out.push_back(entry.path().filename().string());
        }
    }
    return out;
}

void create_profile(std::string_view name) {
    require_nonempty(name, "create_profile");
    const auto target = get_profiles_root() / std::string(name);
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        throw std::runtime_error(
            "hermes::profile::create_profile: mkdir failed for " +
            target.string() + ": " + ec.message());
    }

    const auto cfg_dst = target / "config.yaml";
    if (fs::exists(cfg_dst, ec)) {
        return;  // Already seeded.
    }

    const auto cfg_src = hermes::core::path::get_default_hermes_root() /
                         "config.yaml";
    if (fs::exists(cfg_src, ec) && !ec) {
        auto maybe = hermes::core::atomic_io::atomic_read(cfg_src);
        if (maybe.has_value()) {
            hermes::core::atomic_io::atomic_write(cfg_dst, *maybe);
            return;
        }
    }

    // Fallback: empty config file so downstream loads don't 404.
    hermes::core::atomic_io::atomic_write(cfg_dst, std::string{});
}

void delete_profile(std::string_view name) {
    require_nonempty(name, "delete_profile");
    if (is_active(name)) {
        throw std::runtime_error(
            std::string("hermes::profile::delete_profile: cannot delete the "
                        "currently-active profile: ") +
            std::string(name));
    }
    const auto target = get_profiles_root() / std::string(name);
    std::error_code ec;
    if (!fs::exists(target, ec)) {
        return;  // Idempotent.
    }
    fs::remove_all(target, ec);
    if (ec) {
        throw std::runtime_error(
            "hermes::profile::delete_profile: remove_all failed for " +
            target.string() + ": " + ec.message());
    }
}

void rename_profile(std::string_view old_name, std::string_view new_name) {
    require_nonempty(old_name, "rename_profile");
    require_nonempty(new_name, "rename_profile");
    if (is_active(old_name)) {
        throw std::runtime_error(
            std::string("hermes::profile::rename_profile: cannot rename the "
                        "currently-active profile: ") +
            std::string(old_name));
    }
    const auto root = get_profiles_root();
    const auto src = root / std::string(old_name);
    const auto dst = root / std::string(new_name);
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        throw std::runtime_error(
            "hermes::profile::rename_profile: source does not exist: " +
            src.string());
    }
    if (fs::exists(dst, ec)) {
        throw std::runtime_error(
            "hermes::profile::rename_profile: destination already exists: " +
            dst.string());
    }
    fs::rename(src, dst, ec);
    if (ec) {
        throw std::runtime_error(
            "hermes::profile::rename_profile: rename failed: " + ec.message());
    }
}

}  // namespace hermes::profile
