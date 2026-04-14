// C++17 port of hermes_cli/claw.py — adds `status` and `cleanup`
// on top of the existing `claw_migrate` implementation.
#include "hermes/cli/claw_cmd.hpp"

#include "hermes/cli/claw_migrate.hpp"
#include "hermes/cli/colors.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace hermes::cli::claw_cmd {

namespace fs = std::filesystem;
namespace col = hermes::cli::colors;

namespace {

fs::path home_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
#ifdef _WIN32
        const char* p = std::getenv("USERPROFILE");
        if (p) return fs::path(p);
#endif
        return fs::path("/tmp");
    }
    return fs::path(home);
}

int count_entries_under(const fs::path& p) {
    if (!fs::exists(p)) return 0;
    std::error_code ec;
    int n = 0;
    for (auto it = fs::directory_iterator(p, ec);
         !ec && it != fs::directory_iterator(); ++it) {
        ++n;
    }
    return n;
}

std::string timestamp_suffix() {
    auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_utc);
    return std::string(buf);
}

bool confirm(const std::string& q, bool default_yes) {
    std::cout << "  " << col::yellow(q)
              << (default_yes ? " [Y/n]: " : " [y/N]: ");
    std::string s;
    if (!std::getline(std::cin, s)) {
        std::cout << "\n";
        return default_yes;
    }
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    auto trimmed = s;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();
    if (trimmed.empty()) return default_yes;
    return trimmed == "y" || trimmed == "yes";
}

}  // namespace

OpenClawScan scan_openclaw(const fs::path& root_in) {
    OpenClawScan scan;
    scan.root = root_in.empty() ? (home_dir() / ".openclaw") : root_in;
    if (!fs::exists(scan.root)) {
        scan.findings.push_back("(not found)");
        return scan;
    }
    scan.exists = true;
    auto soul = scan.root / "SOUL.md";
    auto memory = scan.root / "MEMORY.md";
    auto user = scan.root / "USER.md";
    auto skills = scan.root / "skills";
    auto env = scan.root / ".env";
    auto sessions = scan.root / "sessions";

    scan.has_soul = fs::exists(soul);
    scan.has_memory = fs::exists(memory);
    scan.has_user = fs::exists(user);
    scan.has_skills = fs::exists(skills) && fs::is_directory(skills);
    scan.has_env = fs::exists(env);
    if (scan.has_skills) scan.skill_count = count_entries_under(skills);
    if (fs::exists(sessions)) scan.session_count = count_entries_under(sessions);

    if (scan.has_soul) scan.findings.push_back("SOUL.md present");
    if (scan.has_memory) scan.findings.push_back("MEMORY.md present");
    if (scan.has_user) scan.findings.push_back("USER.md present");
    if (scan.has_skills) scan.findings.push_back(
        std::to_string(scan.skill_count) + " skills under skills/");
    if (scan.has_env) scan.findings.push_back(".env file present");
    if (scan.session_count > 0)
        scan.findings.push_back(std::to_string(scan.session_count) +
                                " sessions");
    if (scan.findings.empty())
        scan.findings.push_back("Root exists but is empty");
    return scan;
}

std::vector<fs::path> find_openclaw_roots() {
    std::vector<fs::path> out;
    auto h = home_dir();
    for (const char* name : {".openclaw", ".openclaw-old", ".openclaw-backup"}) {
        fs::path p = h / name;
        if (fs::exists(p)) out.push_back(p);
    }
    return out;
}

fs::path archive_directory(const fs::path& src, bool dry_run) {
    if (!fs::exists(src)) return {};
    fs::path dest = src.parent_path() /
                    (src.filename().string() + "-archive-" + timestamp_suffix());
    if (dry_run) {
        std::cout << "  [dry-run] would archive " << src << " -> " << dest
                  << "\n";
        return dest;
    }
    std::error_code ec;
    fs::rename(src, dest, ec);
    if (ec) {
        // Fall back to copy + remove if rename spans volumes.
        fs::copy(src, dest,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 ec);
        if (ec) {
            std::cerr << "  archive failed: " << ec.message() << "\n";
            return {};
        }
        fs::remove_all(src, ec);
    }
    return dest;
}

// -----------------------------------------------------------------
// Subcommand handlers
// -----------------------------------------------------------------

int cmd_status(const std::vector<std::string>& /*argv*/) {
    auto roots = find_openclaw_roots();
    if (roots.empty()) {
        std::cout << "\n  No OpenClaw installations found under $HOME.\n\n";
        return 0;
    }
    std::cout << "\n  " << col::cyan("OpenClaw installations detected:")
              << "\n\n";
    for (const auto& r : roots) {
        auto scan = scan_openclaw(r);
        std::cout << "  " << col::bold(r.string()) << "\n";
        for (const auto& f : scan.findings) {
            std::cout << "    - " << f << "\n";
        }
        std::cout << "\n";
    }
    std::cout << "  Run 'hermes claw migrate --dry-run' to preview "
                 "imports.\n\n";
    return 0;
}

int cmd_cleanup(const std::vector<std::string>& argv) {
    bool dry_run = false;
    bool yes = false;
    for (const auto& a : argv) {
        if (a == "--dry-run") dry_run = true;
        else if (a == "--yes" || a == "-y") yes = true;
    }
    auto roots = find_openclaw_roots();
    if (roots.empty()) {
        std::cout << "  Nothing to clean up — no OpenClaw directories "
                     "found.\n";
        return 0;
    }
    std::cout << "\n  The following directories will be archived:\n\n";
    for (const auto& r : roots) {
        std::cout << "    - " << r << "\n";
    }
    std::cout << "\n";
    if (!dry_run && !yes && !confirm("Proceed with archival?", true)) {
        std::cout << "  Cancelled.\n";
        return 0;
    }
    int archived = 0;
    for (const auto& r : roots) {
        auto dest = archive_directory(r, dry_run);
        if (!dest.empty()) {
            std::cout << "  " << col::green("\xe2\x9c\x93 ") << r << " -> "
                      << dest << "\n";
            ++archived;
        }
    }
    std::cout << "\n  Archived " << archived << " directory(ies).\n\n";
    return 0;
}

int cmd_migrate(const std::vector<std::string>& argv) {
    claw::MigrateOptions opts;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const auto& a = argv[i];
        if (a == "--dry-run") opts.dry_run = true;
        else if (a == "--overwrite") opts.overwrite = true;
        else if (a == "--preset" && i + 1 < argv.size()) opts.preset = argv[++i];
        else if (a == "--openclaw-dir" && i + 1 < argv.size())
            opts.openclaw_dir = argv[++i];
    }
    auto r = claw::migrate(opts);
    std::cout << "\n  " << col::cyan("Migration summary:") << "\n";
    std::cout << "    imported: " << r.imported.size() << "\n";
    std::cout << "    skipped:  " << r.skipped.size() << "\n";
    std::cout << "    errors:   " << r.errors.size() << "\n\n";
    for (const auto& e : r.errors) {
        std::cout << "    " << col::red("\xe2\x9c\x97 ") << e << "\n";
    }
    return r.errors.empty() ? 0 : 1;
}

namespace {
void print_help() {
    std::cout << "hermes claw — OpenClaw migration utility\n\n"
              << "Usage:\n"
              << "  hermes claw status\n"
              << "  hermes claw migrate [--dry-run] [--overwrite] "
                 "[--preset full|user-data|no-secrets]\n"
              << "  hermes claw cleanup [--dry-run] [--yes]\n";
}
}  // namespace

int run(int argc, char* argv[]) {
    if (argc <= 2) {
        print_help();
        return 0;
    }
    std::string sub = argv[2];
    std::vector<std::string> rest;
    for (int i = 3; i < argc; ++i) rest.emplace_back(argv[i]);

    if (sub == "status") return cmd_status(rest);
    if (sub == "cleanup") return cmd_cleanup(rest);
    if (sub == "migrate") return cmd_migrate(rest);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_help();
        return 0;
    }
    std::cerr << "Unknown claw subcommand: " << sub << "\n";
    print_help();
    return 1;
}

}  // namespace hermes::cli::claw_cmd
