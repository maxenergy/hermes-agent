// Pure-logic helpers driving `hermes gateway install/start` flows.
#include "hermes/cli/gateway_helpers.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <regex>
#include <sstream>

namespace hermes::cli::gateway_helpers {

namespace fs = std::filesystem;

namespace {

std::string sha256_hex_prefix(const std::string& s, std::size_t prefix) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
    std::ostringstream oss;
    for (unsigned char b : hash) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(b);
    }
    return oss.str().substr(0, prefix);
}

std::string strip(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    s.erase(0, i);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

bool path_starts_with(const fs::path& p, const fs::path& prefix) {
    auto pi = p.begin(); auto pe = p.end();
    auto qi = prefix.begin(); auto qe = prefix.end();
    for (; qi != qe; ++qi, ++pi) {
        if (pi == pe) return false;
        if (*pi != *qi) return false;
    }
    return true;
}

fs::path strip_prefix(const fs::path& p, const fs::path& prefix) {
    auto pi = p.begin(); auto pe = p.end();
    auto qi = prefix.begin(); auto qe = prefix.end();
    while (qi != qe) {
        ++pi;
        ++qi;
    }
    fs::path out;
    for (; pi != pe; ++pi) out /= *pi;
    return out;
}

}  // namespace

// -------------------------------------------------------------------------
// Profile suffix.
// -------------------------------------------------------------------------

bool is_valid_profile_name(const std::string& name) {
    static const std::regex re("^[a-z0-9][a-z0-9_-]{0,63}$");
    return std::regex_match(name, re);
}

std::string profile_suffix(const fs::path& hermes_home,
                           const fs::path& default_root) {
    fs::path home = hermes_home.lexically_normal();
    fs::path def = default_root.lexically_normal();
    if (home == def) return "";

    fs::path profiles_root = (def / "profiles").lexically_normal();
    if (path_starts_with(home, profiles_root)) {
        auto rel = strip_prefix(home, profiles_root);
        if (std::distance(rel.begin(), rel.end()) == 1) {
            std::string name = rel.begin()->string();
            if (is_valid_profile_name(name)) return name;
        }
    }
    return sha256_hex_prefix(home.string(), 8);
}

std::string profile_arg(const fs::path& hermes_home,
                        const fs::path& default_root) {
    fs::path home = hermes_home.lexically_normal();
    fs::path def = default_root.lexically_normal();
    if (home == def) return "";

    fs::path profiles_root = (def / "profiles").lexically_normal();
    if (path_starts_with(home, profiles_root)) {
        auto rel = strip_prefix(home, profiles_root);
        if (std::distance(rel.begin(), rel.end()) == 1) {
            std::string name = rel.begin()->string();
            if (is_valid_profile_name(name)) return std::string("--profile ") + name;
        }
    }
    return "";
}

std::string service_name(const fs::path& hermes_home,
                         const fs::path& default_root) {
    auto suffix = profile_suffix(hermes_home, default_root);
    if (suffix.empty()) return kServiceBase;
    return std::string(kServiceBase) + "-" + suffix;
}

std::string launchd_label(const fs::path& hermes_home,
                          const fs::path& default_root) {
    auto suffix = profile_suffix(hermes_home, default_root);
    if (suffix.empty()) return kLaunchdLabelBase;
    return std::string(kLaunchdLabelBase) + "." + suffix;
}

fs::path user_systemd_unit_path(const fs::path& config_home,
                                const fs::path& hermes_home,
                                const fs::path& default_root) {
    return config_home / "systemd" / "user" /
           (service_name(hermes_home, default_root) + ".service");
}

fs::path system_systemd_unit_path(const fs::path& hermes_home,
                                  const fs::path& default_root) {
    return fs::path("/etc/systemd/system") /
           (service_name(hermes_home, default_root) + ".service");
}

// -------------------------------------------------------------------------
// Path remapping.
// -------------------------------------------------------------------------

std::string remap_path_for_user(const std::string& path,
                                const fs::path& current_home,
                                const fs::path& target_home) {
    fs::path resolved = fs::path(path).lexically_normal();
    fs::path home = current_home.lexically_normal();
    if (path_starts_with(resolved, home)) {
        auto rel = strip_prefix(resolved, home);
        return (target_home / rel).string();
    }
    return resolved.string();
}

fs::path hermes_home_for_target_user(const fs::path& current_hermes,
                                     const fs::path& current_home,
                                     const fs::path& target_home) {
    fs::path cur = current_hermes.lexically_normal();
    fs::path cur_default = (current_home / ".hermes").lexically_normal();
    fs::path tgt_default = target_home / ".hermes";
    if (cur == cur_default) return tgt_default;
    if (path_starts_with(cur, cur_default)) {
        auto rel = strip_prefix(cur, cur_default);
        return tgt_default / rel;
    }
    return cur;
}

std::vector<std::string> build_user_local_paths(
    const fs::path& home,
    const std::vector<std::string>& path_entries,
    const ExistenceCheck& existence_check) {
    auto exists = [&](const fs::path& p) {
        return existence_check ? existence_check(p) : fs::exists(p);
    };
    const std::array<fs::path, 4> candidates = {
        home / ".local" / "bin",
        home / ".cargo" / "bin",
        home / "go" / "bin",
        home / ".npm-global" / "bin",
    };
    std::vector<std::string> out;
    for (const auto& cand : candidates) {
        const std::string s = cand.string();
        if (std::find(path_entries.begin(), path_entries.end(), s) !=
            path_entries.end()) {
            continue;
        }
        if (exists(cand)) out.push_back(s);
    }
    return out;
}

// -------------------------------------------------------------------------
// Service definition normaliser.
// -------------------------------------------------------------------------

std::string normalize_service_definition(const std::string& text) {
    auto trimmed = strip(text);
    std::vector<std::string> out_lines;
    std::string cur;
    for (char c : trimmed) {
        if (c == '\n') {
            // rstrip
            while (!cur.empty() &&
                   std::isspace(static_cast<unsigned char>(cur.back()))) {
                cur.pop_back();
            }
            out_lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    while (!cur.empty() &&
           std::isspace(static_cast<unsigned char>(cur.back()))) {
        cur.pop_back();
    }
    out_lines.push_back(cur);
    std::string out;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        if (i) out += "\n";
        out += out_lines[i];
    }
    return out;
}

// -------------------------------------------------------------------------
// Restart drain timeout.
// -------------------------------------------------------------------------

std::optional<int> parse_restart_drain_timeout(const std::string& raw) {
    auto s = strip(raw);
    if (s.empty()) return std::nullopt;
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return std::nullopt;
        if (v <= 0) return std::nullopt;
        if (v > std::numeric_limits<int>::max()) return std::nullopt;
        return static_cast<int>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// -------------------------------------------------------------------------
// Discord allowlist split.
// -------------------------------------------------------------------------

std::vector<std::string> split_allowlist(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') {
            auto trimmed = strip(cur);
            if (!trimmed.empty()) out.push_back(trimmed);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    auto trimmed = strip(cur);
    if (!trimmed.empty()) out.push_back(trimmed);
    return out;
}

}  // namespace hermes::cli::gateway_helpers
