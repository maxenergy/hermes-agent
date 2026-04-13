#include "hermes/cli/update_prompt.hpp"

#include "hermes/core/atomic_io.hpp"
#include "hermes/core/env.hpp"
#include "hermes/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace hermes::cli::update_prompt {

namespace fs = std::filesystem;

namespace {

bool is_tty_stdin() {
#ifdef _WIN32
    return false;  // Conservative; daemon mode rarely needs the prompt.
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string strip_v(std::string_view v) {
    if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) {
        return std::string(v.substr(1));
    }
    return std::string(v);
}

// Split on '.' into numeric components plus a trailing pre-release tag.
struct ParsedVersion {
    std::vector<int> nums;
    std::string pre;     // text after first '-' or '+'
    bool ok = false;
};
ParsedVersion parse_ver(std::string_view raw) {
    ParsedVersion out;
    std::string s = strip_v(raw);
    auto dash = s.find_first_of("-+");
    if (dash != std::string::npos) {
        out.pre = s.substr(dash + 1);
        s.resize(dash);
    }
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, '.')) {
        try {
            out.nums.push_back(std::stoi(part));
        } catch (...) {
            return out;  // ok stays false
        }
    }
    out.ok = !out.nums.empty();
    return out;
}

}  // namespace

bool is_newer_version(std::string_view local, std::string_view remote) {
    auto L = parse_ver(local);
    auto R = parse_ver(remote);
    if (!L.ok || !R.ok) {
        // Fallback: lexicographic.
        return strip_v(remote) > strip_v(local);
    }
    size_t n = std::max(L.nums.size(), R.nums.size());
    for (size_t i = 0; i < n; ++i) {
        int li = i < L.nums.size() ? L.nums[i] : 0;
        int ri = i < R.nums.size() ? R.nums[i] : 0;
        if (ri > li) return true;
        if (ri < li) return false;
    }
    // Numeric segments equal.  A bare release outranks a pre-release.
    bool l_pre = !L.pre.empty();
    bool r_pre = !R.pre.empty();
    if (l_pre && !r_pre) return true;   // local is rc, remote is full
    if (!l_pre && r_pre) return false;  // local already full
    return false;
}

std::optional<LatestManifest> parse_latest_manifest(std::string_view body) {
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_object()) return std::nullopt;
        // Accept either {"version": "..."} or {"latest_version": "..."}.
        std::string v;
        if (j.contains("version") && j["version"].is_string()) {
            v = j["version"].get<std::string>();
        } else if (j.contains("latest_version") &&
                   j["latest_version"].is_string()) {
            v = j["latest_version"].get<std::string>();
        }
        if (v.empty()) return std::nullopt;
        LatestManifest m;
        m.version = std::move(v);
        m.url = j.value("url", "");
        m.notes = j.value("notes", "");
        return m;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

fs::path default_state_path() {
    auto home = hermes::core::path::get_hermes_home();
    auto dir = home / "state";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "update.json";
}

ThrottleState load_throttle(const fs::path& state_path) {
    ThrottleState s;
    std::ifstream f(state_path);
    if (!f) return s;
    try {
        nlohmann::json j;
        f >> j;
        if (!j.is_object()) return s;
        s.exists = true;
        if (j.contains("last_update_check_at") &&
            j["last_update_check_at"].is_number_integer()) {
            std::int64_t epoch = j["last_update_check_at"].get<std::int64_t>();
            s.last_check = std::chrono::system_clock::time_point(
                std::chrono::seconds(epoch));
        }
        s.last_seen_version = j.value("last_seen_version", "");
    } catch (...) {
        // Corrupt — treat as missing.
        s = {};
    }
    return s;
}

void save_throttle(const fs::path& state_path, const ThrottleState& s) {
    nlohmann::json j;
    j["last_update_check_at"] =
        std::chrono::duration_cast<std::chrono::seconds>(
            s.last_check.time_since_epoch())
            .count();
    j["last_seen_version"] = s.last_seen_version;
    std::error_code ec;
    fs::create_directories(state_path.parent_path(), ec);
    // Atomic write so we never leave a half-written JSON.
    hermes::core::atomic_io::atomic_write(state_path, j.dump(2));
}

UpdateOutcome maybe_prompt_update(UpdateConfig cfg) {
    UpdateOutcome out;

    // 1) Skip gates.
    if (!cfg.force) {
        if (cfg.no_update_check_flag) {
            out.skipped_no_tty = false;
            out.detail = "skipped: --no-update-check";
            return out;
        }
        if (hermes::core::env::env_var_enabled("CI")) {
            out.skipped_no_tty = true;
            out.detail = "skipped: CI=true";
            return out;
        }
        if (!is_tty_stdin()) {
            out.skipped_no_tty = true;
            out.detail = "skipped: stdin not a tty";
            return out;
        }
    }

    fs::path state_path = cfg.state_path_override.empty()
                              ? default_state_path()
                              : cfg.state_path_override;

    // 2) Throttle.
    auto state = load_throttle(state_path);
    if (state.exists && !cfg.bypass_throttle) {
        auto age = cfg.now - state.last_check;
        if (age >= std::chrono::seconds::zero() && age < cfg.throttle) {
            out.throttled = true;
            out.detail = "throttled";
            return out;
        }
    }

    // 3) Fetch.
    std::optional<std::string> body;
    if (cfg.fetch) {
        body = cfg.fetch(cfg.manifest_url);
    }
    // Always update last-check stamp so transport failures still
    // throttle for the next 24h (avoid hammering the network).
    state.last_check = cfg.now;
    if (!body) {
        save_throttle(state_path, state);
        out.detail = "fetch failed";
        return out;
    }
    auto manifest = parse_latest_manifest(*body);
    if (!manifest) {
        save_throttle(state_path, state);
        out.detail = "manifest parse failed";
        return out;
    }
    state.last_seen_version = manifest->version;
    out.latest_version = manifest->version;

    // 4) Compare.
    if (!is_newer_version(cfg.current_version, manifest->version)) {
        save_throttle(state_path, state);
        out.detail = "up to date";
        return out;
    }

    // 5) Prompt.
    std::string prompt = "Update from v" + cfg.current_version + " -> v" +
                         manifest->version + "? [y/N]: ";
    if (cfg.write_line) {
        cfg.write_line(prompt);
    } else {
        std::cout << prompt << std::flush;
    }
    out.prompted = true;

    std::optional<std::string> answer;
    if (cfg.read_line) {
        answer = cfg.read_line();
    } else {
        std::string line;
        if (std::getline(std::cin, line)) answer = line;
    }
    if (answer) {
        std::string a = trim(*answer);
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        out.user_accepted = (a == "y" || a == "yes");
    }

    save_throttle(state_path, state);
    out.detail = out.user_accepted ? "accepted" : "declined";
    return out;
}

}  // namespace hermes::cli::update_prompt
