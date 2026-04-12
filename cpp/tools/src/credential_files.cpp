#include "hermes/tools/credential_files.hpp"

#include <cstdlib>
#include <system_error>

namespace hermes::tools {

namespace {

std::filesystem::path home_or_cwd() {
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h);
    }
    return std::filesystem::current_path();
}

}  // namespace

std::filesystem::path hermes_home() {
    if (const char* override_home = std::getenv("HERMES_HOME");
        override_home && *override_home) {
        return std::filesystem::path(override_home);
    }
    return home_or_cwd() / ".hermes";
}

std::filesystem::path hermes_env_file() {
    return hermes_home() / ".env";
}

std::filesystem::path hermes_credentials_dir() {
    return hermes_home() / ".credentials";
}

std::filesystem::path credential_path(std::string_view relative) {
    return hermes_credentials_dir() / std::filesystem::path(std::string(relative));
}

std::vector<std::filesystem::path> list_credential_files() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const auto root = hermes_credentials_dir();
    if (!std::filesystem::exists(root, ec) || ec) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) {
            out.push_back(it->path());
        }
    }
    return out;
}

}  // namespace hermes::tools
