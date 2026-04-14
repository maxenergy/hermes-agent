// pairing — implementation. See pairing.hpp.
#include "hermes/cli/pairing.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace hermes::cli::pairing {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Serialize/parse an ISO-8601 time_point. Never throws — returns epoch
// on parse failure so the caller can still compute a stable age.
std::string iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    if (s.empty()) return std::chrono::system_clock::time_point{};
    std::tm tm_buf{};
    std::istringstream is(s);
    is >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    if (is.fail()) return std::chrono::system_clock::time_point{};
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm_buf);
#else
    std::time_t t = timegm(&tm_buf);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

bool atomic_write(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << content;
        if (!f) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Fall back to copy+remove when rename fails across filesystems.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return false;
    }
    return true;
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

}  // namespace

long PendingEntry::age_minutes(std::chrono::system_clock::time_point now) const {
    auto diff = now - created_at;
    return std::chrono::duration_cast<std::chrono::minutes>(diff).count();
}

std::vector<std::string> known_platforms() {
    return {
        "cli",       "telegram",  "discord",   "slack",
        "whatsapp",  "signal",    "bluebubbles", "email",
        "homeassistant", "mattermost", "matrix", "dingtalk",
        "feishu",    "wecom",     "weixin",    "webhook",
    };
}

std::string canonical_platform(std::string_view platform) {
    return lower(trim(platform));
}

std::string canonical_code(std::string_view code) {
    return upper(trim(code));
}

fs::path default_pairing_dir() {
    auto home = hermes::core::path::get_hermes_home();
    auto dir = home / "pairing";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path pending_path(const fs::path& dir, const std::string& platform) {
    return dir / ("pending_" + platform + ".json");
}

fs::path approved_path(const fs::path& dir, const std::string& platform) {
    return dir / ("approved_" + platform + ".json");
}

std::vector<PendingEntry> read_pending(const fs::path& dir,
                                       const std::string& platform) {
    std::vector<PendingEntry> out;
    auto path = pending_path(dir, platform);
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;
    try {
        auto j = json::parse(read_file(path));
        if (!j.is_array()) return out;
        for (const auto& item : j) {
            PendingEntry e;
            e.platform = platform;
            if (item.contains("code")) e.code = item.value("code", "");
            if (item.contains("user_id")) e.user_id = item.value("user_id", "");
            if (item.contains("user_name")) e.user_name = item.value("user_name", "");
            if (item.contains("created_at")) {
                e.created_at = parse_iso8601(item.value("created_at", ""));
            }
            out.push_back(std::move(e));
        }
    } catch (...) {
        // Swallow — caller sees empty list.
    }
    return out;
}

std::vector<ApprovedEntry> read_approved(const fs::path& dir,
                                         const std::string& platform) {
    std::vector<ApprovedEntry> out;
    auto path = approved_path(dir, platform);
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;
    try {
        auto j = json::parse(read_file(path));
        if (!j.is_array()) return out;
        for (const auto& item : j) {
            ApprovedEntry e;
            e.platform = platform;
            if (item.is_string()) {
                e.user_id = item.get<std::string>();
            } else if (item.is_object()) {
                e.user_id = item.value("user_id", "");
                e.user_name = item.value("user_name", "");
            }
            if (!e.user_id.empty()) out.push_back(std::move(e));
        }
    } catch (...) {
    }
    return out;
}

bool write_pending(const fs::path& dir, const std::string& platform,
                   const std::vector<PendingEntry>& entries) {
    json arr = json::array();
    for (const auto& e : entries) {
        json obj;
        obj["code"] = e.code;
        obj["user_id"] = e.user_id;
        obj["user_name"] = e.user_name;
        obj["created_at"] = iso8601(e.created_at);
        arr.push_back(std::move(obj));
    }
    return atomic_write(pending_path(dir, platform), arr.dump(2));
}

bool write_approved(const fs::path& dir, const std::string& platform,
                    const std::vector<ApprovedEntry>& entries) {
    json arr = json::array();
    for (const auto& e : entries) {
        json obj;
        obj["user_id"] = e.user_id;
        obj["user_name"] = e.user_name;
        arr.push_back(std::move(obj));
    }
    return atomic_write(approved_path(dir, platform), arr.dump(2));
}

std::vector<PendingEntry> list_pending(const fs::path& dir) {
    std::vector<PendingEntry> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        auto name = entry.path().filename().string();
        const std::string prefix = "pending_";
        const std::string suffix = ".json";
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::string platform = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        for (auto& p : read_pending(dir, platform)) {
            out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<ApprovedEntry> list_approved(const fs::path& dir) {
    std::vector<ApprovedEntry> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        auto name = entry.path().filename().string();
        const std::string prefix = "approved_";
        const std::string suffix = ".json";
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::string platform = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        for (auto& p : read_approved(dir, platform)) {
            out.push_back(std::move(p));
        }
    }
    return out;
}

bool revoke(const fs::path& dir, const std::string& platform_in,
            const std::string& user_id) {
    auto platform = canonical_platform(platform_in);
    auto approved = read_approved(dir, platform);
    auto it = std::remove_if(approved.begin(), approved.end(),
                             [&](const ApprovedEntry& e) {
                                 return e.user_id == user_id;
                             });
    if (it == approved.end()) return false;
    approved.erase(it, approved.end());
    return write_approved(dir, platform, approved);
}

std::size_t clear_pending(const fs::path& dir) {
    std::size_t count = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        auto name = entry.path().filename().string();
        const std::string prefix = "pending_";
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        auto p = read_pending(dir, name.substr(prefix.size(),
                                               name.size() - prefix.size() - 5));
        count += p.size();
        std::error_code rmec;
        fs::remove(entry.path(), rmec);
    }
    return count;
}

std::optional<ApprovedEntry> approve(const fs::path& dir,
                                     const std::string& platform_in,
                                     const std::string& code_in,
                                     std::chrono::seconds ttl) {
    auto platform = canonical_platform(platform_in);
    auto code = canonical_code(code_in);
    auto pending = read_pending(dir, platform);
    auto now = std::chrono::system_clock::now();
    std::optional<ApprovedEntry> promoted;
    std::vector<PendingEntry> remaining;
    for (const auto& e : pending) {
        bool expired = (e.created_at.time_since_epoch().count() > 0) &&
                       (now - e.created_at > ttl);
        if (!promoted && e.code == code && !expired) {
            ApprovedEntry ap;
            ap.platform = platform;
            ap.user_id = e.user_id;
            ap.user_name = e.user_name;
            promoted = ap;
            continue;
        }
        if (!expired) remaining.push_back(e);
    }
    if (!promoted) return std::nullopt;

    auto approved = read_approved(dir, platform);
    bool exists = false;
    for (auto& existing : approved) {
        if (existing.user_id == promoted->user_id) {
            existing.user_name = promoted->user_name;
            exists = true;
            break;
        }
    }
    if (!exists) approved.push_back(*promoted);

    write_pending(dir, platform, remaining);
    write_approved(dir, platform, approved);
    return promoted;
}

// --- rendering -------------------------------------------------------------

namespace {
std::string pad(std::string s, std::size_t w) {
    if (s.size() >= w) return s.substr(0, w);
    s.append(w - s.size(), ' ');
    return s;
}
}

std::size_t render_pending_table(std::ostream& out,
                                 const std::vector<PendingEntry>& entries) {
    if (entries.empty()) {
        out << "\n  No pending pairing requests.\n";
        return 0;
    }
    out << "\n  Pending Pairing Requests (" << entries.size() << "):\n";
    out << "  " << pad("Platform", 12) << " " << pad("Code", 10) << " "
        << pad("User ID", 20) << " " << pad("Name", 20) << " Age\n";
    out << "  " << std::string(12, '-') << " " << std::string(10, '-') << " "
        << std::string(20, '-') << " " << std::string(20, '-') << " ---\n";
    for (const auto& e : entries) {
        out << "  " << pad(e.platform, 12) << " " << pad(e.code, 10) << " "
            << pad(e.user_id, 20) << " " << pad(e.user_name, 20) << " "
            << e.age_minutes() << "m ago\n";
    }
    return entries.size();
}

std::size_t render_approved_table(std::ostream& out,
                                  const std::vector<ApprovedEntry>& entries) {
    if (entries.empty()) {
        out << "\n  No approved users.\n";
        return 0;
    }
    out << "\n  Approved Users (" << entries.size() << "):\n";
    out << "  " << pad("Platform", 12) << " " << pad("User ID", 20) << " "
        << pad("Name", 20) << "\n";
    out << "  " << std::string(12, '-') << " " << std::string(20, '-') << " "
        << std::string(20, '-') << "\n";
    for (const auto& e : entries) {
        out << "  " << pad(e.platform, 12) << " " << pad(e.user_id, 20) << " "
            << pad(e.user_name, 20) << "\n";
    }
    return entries.size();
}

int cmd_list(const fs::path& dir, std::ostream& out) {
    auto pending = list_pending(dir);
    auto approved = list_approved(dir);
    if (pending.empty() && approved.empty()) {
        out << "No pairing data found. No one has tried to pair yet~\n";
        return 0;
    }
    render_pending_table(out, pending);
    render_approved_table(out, approved);
    out << "\n";
    return 0;
}

int dispatch(int argc, char** argv) {
    // argv[0] = "hermes", argv[1] = "pairing", argv[2] = subcommand
    if (argc < 3) {
        std::cout << "Usage: hermes pairing {list|approve|revoke|clear-pending}\n";
        return 1;
    }
    std::string sub = argv[2];
    auto dir = default_pairing_dir();

    if (sub == "list") {
        return cmd_list(dir, std::cout);
    }
    if (sub == "approve") {
        if (argc < 5) {
            std::cerr << "Usage: hermes pairing approve <platform> <code>\n";
            return 1;
        }
        auto res = approve(dir, argv[3], argv[4]);
        if (res) {
            std::string display = res->user_name.empty() ? res->user_id :
                (res->user_name + " (" + res->user_id + ")");
            std::cout << "\n  Approved! User " << display << " on "
                      << res->platform << " can now use the bot~\n\n";
            return 0;
        }
        std::cout << "\n  Code '" << canonical_code(argv[4])
                  << "' not found or expired for platform '"
                  << canonical_platform(argv[3]) << "'.\n\n";
        return 1;
    }
    if (sub == "revoke") {
        if (argc < 5) {
            std::cerr << "Usage: hermes pairing revoke <platform> <user_id>\n";
            return 1;
        }
        if (revoke(dir, argv[3], argv[4])) {
            std::cout << "\n  Revoked access for user " << argv[4]
                      << " on " << canonical_platform(argv[3]) << ".\n\n";
            return 0;
        }
        std::cout << "\n  User " << argv[4]
                  << " not found in approved list for "
                  << canonical_platform(argv[3]) << ".\n\n";
        return 1;
    }
    if (sub == "clear-pending") {
        auto n = clear_pending(dir);
        if (n > 0) {
            std::cout << "\n  Cleared " << n
                      << " pending pairing request(s).\n\n";
        } else {
            std::cout << "\n  No pending requests to clear.\n\n";
        }
        return 0;
    }
    std::cerr << "Unknown pairing subcommand: " << sub << "\n";
    return 1;
}

}  // namespace hermes::cli::pairing
