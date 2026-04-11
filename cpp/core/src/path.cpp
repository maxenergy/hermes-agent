#include "hermes/core/path.hpp"

#include <cstdlib>
#include <string>
#include <system_error>

namespace hermes::core::path {

namespace {

fs::path home_dir() {
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') {
        return fs::path(h);
    }
    // Fallback: current working directory rather than throwing.
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    return ec ? fs::path("/") : cwd;
}

}  // namespace

fs::path get_hermes_home() {
    if (const char* h = std::getenv("HERMES_HOME"); h != nullptr && *h != '\0') {
        return fs::path(h);
    }
    return home_dir() / ".hermes";
}

fs::path get_default_hermes_root() {
    return home_dir() / ".hermes";
}

fs::path get_profiles_root() {
    // Deliberately HOME-anchored per Python parity.
    return home_dir() / ".hermes" / "profiles";
}

fs::path get_optional_skills_dir() {
    return get_hermes_home() / "optional-skills";
}

fs::path get_hermes_dir(std::string_view new_subpath, std::string_view old_name) {
    const auto home = get_hermes_home();
    if (!old_name.empty()) {
        const auto old_path = home / std::string(old_name);
        std::error_code ec;
        if (fs::exists(old_path, ec) && !ec) {
            return old_path;
        }
    }
    return home / std::string(new_subpath);
}

std::string display_hermes_home() {
    const auto home = get_hermes_home();
    const auto user_home = home_dir();

    std::error_code ec;
    auto resolved_home = fs::weakly_canonical(home, ec);
    if (ec) {
        resolved_home = home;
    }
    auto resolved_user = fs::weakly_canonical(user_home, ec);
    if (ec) {
        resolved_user = user_home;
    }

    // If `home` is inside `user_home`, produce the `~/...` shorthand.
    auto home_str = resolved_home.string();
    auto user_str = resolved_user.string();
    if (home_str == user_str) {
        return "~";
    }
    if (home_str.size() > user_str.size() &&
        home_str.compare(0, user_str.size(), user_str) == 0 &&
        home_str[user_str.size()] == static_cast<char>(fs::path::preferred_separator)) {
        return "~" + home_str.substr(user_str.size());
    }
    return home_str;
}

}  // namespace hermes::core::path
