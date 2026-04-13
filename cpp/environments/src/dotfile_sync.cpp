#include "hermes/environments/dotfile_sync.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include <unistd.h>

namespace hermes::environments {

namespace {

fs::path default_home() {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
    return fs::path("/root");
}

// Case-insensitive prefix match — ssh_config directives are
// case-insensitive per `man ssh_config`.
bool istarts_with(const std::string& line, const std::string& prefix) {
    if (line.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(line[i])));
        char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

// Strip leading horizontal whitespace but keep the rest intact.
std::string ltrim_copy(const std::string& s) {
    auto it = std::find_if(s.begin(), s.end(), [](char c) {
        return !(c == ' ' || c == '\t');
    });
    return std::string(it, s.end());
}

std::string read_file(const fs::path& p, std::error_code& ec) {
    std::ifstream ifs(p);
    if (!ifs) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool write_file(const fs::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(),
              static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

}  // namespace

DotfileManager::DotfileManager() : DotfileManager(Config{}) {}

DotfileManager::DotfileManager(Config config) : config_(std::move(config)) {
    if (config_.local_home.empty()) {
        config_.local_home = default_home();
    }
    if (config_.files.empty()) {
        config_.files = default_files();
    }

    // Lazily create the staging dir when stage() is called.
}

DotfileManager::~DotfileManager() {
    if (!tmp_root_.empty()) {
        std::error_code ec;
        fs::remove_all(tmp_root_, ec);
    }
}

std::vector<std::string> DotfileManager::default_files() {
    return {
        ".gitconfig",
        ".bashrc",
        ".bash_profile",
        ".zshrc",
        ".inputrc",
        ".tmux.conf",
        ".vimrc",
        ".config/git/config",
        ".ssh/config",
        ".ssh/known_hosts",
    };
}

std::string DotfileManager::sanitize_ssh_config(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    std::stringstream ss(input);
    std::string line;
    static const char* kBanned[] = {
        "IdentityFile",
        "CertificateFile",
        "ControlPath",
        "ControlMaster",
        "ControlPersist",
        "IdentityAgent",
    };
    while (std::getline(ss, line)) {
        auto trimmed = ltrim_copy(line);
        bool banned = false;
        for (const char* kw : kBanned) {
            if (istarts_with(trimmed, kw)) {
                // Require that the next char is whitespace or '=' so
                // e.g. `IdentityFileX` (nonexistent, but be safe) would
                // not match.
                auto n = std::strlen(kw);
                if (trimmed.size() == n || trimmed[n] == ' ' ||
                    trimmed[n] == '\t' || trimmed[n] == '=') {
                    banned = true;
                    break;
                }
            }
        }
        if (!banned) {
            out += line;
            out.push_back('\n');
        }
    }
    // Preserve lack of trailing newline in rare cases.
    if (!input.empty() && input.back() != '\n' && !out.empty() &&
        out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::vector<DotfileManager::Staged> DotfileManager::stage() {
    std::vector<Staged> result;

    for (const auto& rel : config_.files) {
        fs::path local = config_.local_home / rel;
        std::error_code ec;
        if (!fs::exists(local, ec) || ec) continue;
        if (!fs::is_regular_file(local, ec) || ec) continue;

        bool needs_sanitize = (rel == ".ssh/config");
        fs::path staged_local = local;

        if (needs_sanitize) {
            std::error_code read_ec;
            auto content = read_file(local, read_ec);
            if (read_ec) continue;
            auto sanitized = sanitize_ssh_config(content);

            if (tmp_root_.empty()) {
                char tpl[] = "/tmp/hermes-dotfiles-XXXXXX";
                if (::mkdtemp(tpl) != nullptr) {
                    tmp_root_ = tpl;
                }
            }
            if (tmp_root_.empty()) continue;
            staged_local = tmp_root_ / "ssh_config";
            if (!write_file(staged_local, sanitized)) continue;
        }

        Staged s;
        s.local_path = staged_local;
        s.remote_path = config_.remote_home / rel;
        s.sanitized = needs_sanitize;
        result.push_back(std::move(s));
    }
    return result;
}

std::size_t DotfileManager::upload(CopyFn copy_fn) {
    std::size_t ok = 0;
    auto staged = stage();
    for (const auto& s : staged) {
        if (copy_fn(s.local_path, s.remote_path)) {
            ++ok;
        }
    }
    return ok;
}

}  // namespace hermes::environments
