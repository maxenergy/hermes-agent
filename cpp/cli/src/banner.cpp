// banner — implementation. See banner.hpp for API.
#include "hermes/cli/banner.hpp"
#include "hermes/core/path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <cstdio>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hermes::cli::banner {

namespace fs = std::filesystem;

// --- ASCII art constants ----------------------------------------------------

const std::array<std::string, 6> kHermesLogo{{
    "██╗  ██╗███████╗██████╗ ███╗   ███╗███████╗███████╗       █████╗  ██████╗ ███████╗███╗   ██╗████████╗",
    "██║  ██║██╔════╝██╔══██╗████╗ ████║██╔════╝██╔════╝      ██╔══██╗██╔════╝ ██╔════╝████╗  ██║╚══██╔══╝",
    "███████║█████╗  ██████╔╝██╔████╔██║█████╗  ███████╗█████╗███████║██║  ███╗█████╗  ██╔██╗ ██║   ██║   ",
    "██╔══██║██╔══╝  ██╔══██╗██║╚██╔╝██║██╔══╝  ╚════██║╚════╝██╔══██║██║   ██║██╔══╝  ██║╚██╗██║   ██║   ",
    "██║  ██║███████╗██║  ██║██║ ╚═╝ ██║███████╗███████║      ██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║   ",
    "╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝╚══════╝╚══════╝      ╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝   ",
}};

const std::array<std::string, 15> kHermesCaduceus{{
    "          ⠀⠀⠀⠀⠀⢀⣀⡀⠀⣀⣀⠀⢀⣀⡀⠀⠀⠀⠀⠀",
    "      ⢀⣠⣴⣾⣿⣿⣇⠸⣿⣿⠇⣸⣿⣿⣷⣦⣄⡀",
    "  ⢀⣠⣴⣶⠿⠋⣩⡿⣿⡿⠻⣿⡇⢠⡄⢸⣿⠟⢿⣿⢿⣍⠙⠿⣶⣦⣄⡀",
    "     ⠉⠉⠁⠶⠟⠋⠀⠉⠀⢀⣈⣁⡈⢁⣈⣁⡀⠀⠉⠀⠙⠻⠶⠈⠉",
    "               ⣴⣿⡿⠛⢁⡈⠛⢿⣿⣦",
    "               ⠿⣿⣦⣤⣈⠁⢠⣴⣿⠿",
    "                 ⠈⠉⠻⢿⣿⣦⡉⠁",
    "                    ⠘⢷⣦⣈⠛⠃",
    "                 ⢠⣴⠦⠈⠙⠿⣦⡄",
    "                 ⠸⣿⣤⡈⠁⢤⣿⠇",
    "                    ⠉⠛⠷⠄",
    "                ⢀⣀⠑⢶⣄⡀",
    "                ⣿⠁⢰⡆⠈⡿",
    "                ⠈⠳⠈⣡⠞⠁",
    "                   ⠀⠀",
}};

// --- Formatting helpers -----------------------------------------------------

std::string format_context_length(std::size_t tokens) {
    auto fmt_scale = [](double val, const char* suffix) {
        double rounded = std::round(val);
        if (std::abs(val - rounded) < 0.05) {
            std::ostringstream o;
            o << static_cast<long long>(rounded) << suffix;
            return o.str();
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f%s", val, suffix);
        return std::string(buf);
    };

    if (tokens >= 1'000'000) {
        return fmt_scale(static_cast<double>(tokens) / 1'000'000.0, "M");
    }
    if (tokens >= 1'000) {
        return fmt_scale(static_cast<double>(tokens) / 1'000.0, "K");
    }
    return std::to_string(tokens);
}

std::string display_toolset_name(std::string_view toolset_name) {
    if (toolset_name.empty()) {
        return "unknown";
    }
    const std::string suffix = "_tools";
    if (toolset_name.size() > suffix.size() &&
        toolset_name.substr(toolset_name.size() - suffix.size()) == suffix) {
        return std::string(
            toolset_name.substr(0, toolset_name.size() - suffix.size()));
    }
    return std::string(toolset_name);
}

std::string short_model_name(std::string_view model, std::size_t max_len) {
    std::string s(model);
    auto slash = s.find_last_of('/');
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    const std::string gguf = ".gguf";
    if (s.size() > gguf.size() &&
        s.substr(s.size() - gguf.size()) == gguf) {
        s.resize(s.size() - gguf.size());
    }
    if (max_len > 3 && s.size() > max_len) {
        s = s.substr(0, max_len - 3) + "...";
    }
    return s;
}

std::string elide_csv(const std::vector<std::string>& names,
                      std::size_t max_width) {
    if (names.empty()) return "";
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string sep = (i == 0) ? "" : ", ";
        if (out.size() + sep.size() + names[i].size() + 4 > max_width &&
            i + 1 < names.size()) {
            if (!out.empty()) out += ", ...";
            return out;
        }
        out += sep + names[i];
    }
    return out;
}

// --- Skill grouping ---------------------------------------------------------

std::map<std::string, std::vector<std::string>> group_skills_by_category(
    const std::vector<std::pair<std::string, std::string>>& skills) {
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& [name, category] : skills) {
        std::string cat = category.empty() ? "general" : category;
        out[cat].push_back(name);
    }
    return out;
}

CategoryPick pick_categories(
    const std::map<std::string, std::vector<std::string>>& grouped,
    std::size_t max_categories) {
    CategoryPick out;
    std::vector<std::string> keys;
    keys.reserve(grouped.size());
    for (const auto& kv : grouped) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    if (keys.size() <= max_categories) {
        out.visible = std::move(keys);
        return out;
    }
    out.visible.assign(keys.begin(), keys.begin() + max_categories);
    out.remaining = keys.size() - max_categories;
    return out;
}

std::string summarise_names(const std::vector<std::string>& names,
                            std::size_t max_show) {
    if (names.empty()) return "";
    if (names.size() <= max_show) {
        std::string out;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) out += ", ";
            out += names[i];
        }
        return out;
    }
    std::string out;
    for (std::size_t i = 0; i < max_show; ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    out += " +" + std::to_string(names.size() - max_show) + " more";
    return out;
}

// --- Git helpers ------------------------------------------------------------

namespace {

// Capture stdout of `argv` with a CWD. Returns pair{exit_code, output}.
std::pair<int, std::string> run_git(
    const std::filesystem::path& cwd,
    const std::vector<std::string>& args,
    int timeout_seconds = 5) {
    (void)timeout_seconds;  // we rely on git's own -q / ref-only ops
#if defined(_WIN32)
    // Minimal Windows fallback — use system(); no timeout.
    std::string cmdline = "cd /d \"" + cwd.string() + "\" && git";
    for (const auto& a : args) cmdline += " \"" + a + "\"";
    cmdline += " 2>NUL";
    FILE* pipe = _popen(cmdline.c_str(), "r");
    if (!pipe) return {-1, ""};
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    int rc = _pclose(pipe);
    return {rc, out};
#else
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {-1, ""};
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {-1, ""};
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], 1);
        int devnull = ::open("/dev/null", 1);
        if (devnull >= 0) ::dup2(devnull, 2);
        ::close(pipefd[1]);
        std::error_code ec;
        std::filesystem::current_path(cwd, ec);
        std::vector<const char*> argv;
        argv.push_back("git");
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        ::execvp("git", const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, buf + n);
    }
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {rc, out};
#endif
}

std::string strip(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' ||
            s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t start = 0;
    while (start < s.size() &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\n')) {
        ++start;
    }
    return s.substr(start);
}

}  // namespace

std::optional<fs::path> resolve_repo_dir() {
    fs::path home = hermes::core::path::get_hermes_home();
    fs::path candidate = home / "hermes-agent";
    std::error_code ec;
    if (fs::exists(candidate / ".git", ec)) return candidate;
    // Fall back to CWD or exe parent — caller will override for tests.
    fs::path cwd = fs::current_path(ec);
    while (!cwd.empty() && cwd != cwd.root_path()) {
        if (fs::exists(cwd / ".git", ec)) return cwd;
        cwd = cwd.parent_path();
    }
    return std::nullopt;
}

std::optional<std::string> git_short_hash(const fs::path& repo_dir,
                                          const std::string& revision) {
    auto [rc, out] = run_git(repo_dir, {"rev-parse", "--short=8", revision});
    if (rc != 0) return std::nullopt;
    std::string s = strip(out);
    if (s.empty()) return std::nullopt;
    return s;
}

std::optional<GitBannerState> get_git_banner_state(
    const std::optional<fs::path>& repo_dir) {
    fs::path dir;
    if (repo_dir) {
        dir = *repo_dir;
    } else {
        auto resolved = resolve_repo_dir();
        if (!resolved) return std::nullopt;
        dir = *resolved;
    }

    auto upstream = git_short_hash(dir, "origin/main");
    auto local = git_short_hash(dir, "HEAD");
    if (!upstream || !local) return std::nullopt;

    GitBannerState state;
    state.upstream = *upstream;
    state.local = *local;

    auto [rc, out] = run_git(dir, {"rev-list", "--count", "origin/main..HEAD"});
    if (rc == 0) {
        try {
            state.ahead = static_cast<std::size_t>(std::stoul(strip(out)));
        } catch (...) {
            state.ahead = 0;
        }
    }
    return state;
}

std::string format_banner_version_label(const std::string& version,
                                        const std::string& release_date) {
    std::string base = "Hermes Agent v" + version + " (" + release_date + ")";
    auto state = get_git_banner_state();
    if (!state) return base;
    if (state->ahead == 0 || state->upstream == state->local) {
        return base + " · upstream " + state->upstream;
    }
    const char* word = (state->ahead == 1) ? "commit" : "commits";
    return base + " · upstream " + state->upstream +
           " · local " + state->local +
           " (+" + std::to_string(state->ahead) + " carried " + word + ")";
}

std::string format_summary_line(std::size_t tool_count,
                                std::size_t skill_count,
                                std::size_t mcp_connected,
                                bool include_help_hint) {
    std::vector<std::string> parts;
    parts.push_back(std::to_string(tool_count) + " tools");
    parts.push_back(std::to_string(skill_count) + " skills");
    if (mcp_connected > 0) {
        parts.push_back(std::to_string(mcp_connected) + " MCP servers");
    }
    if (include_help_hint) parts.push_back("/help for commands");
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " · ";
        out += parts[i];
    }
    return out;
}

std::string format_update_warning(std::size_t behind,
                                  const std::string& update_cmd) {
    if (behind == 0) return "";
    const char* word = (behind == 1) ? "commit" : "commits";
    return "⚠ " + std::to_string(behind) + " " + word +
           " behind — run " + update_cmd + " to update";
}

// --- Update cache -----------------------------------------------------------

std::optional<UpdateCache> read_update_cache(const fs::path& cache_file) {
    std::error_code ec;
    if (!fs::exists(cache_file, ec)) return std::nullopt;
    std::ifstream f(cache_file);
    if (!f) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    try {
        auto j = nlohmann::json::parse(content);
        UpdateCache out;
        if (j.contains("ts") && j["ts"].is_number()) {
            out.timestamp = std::chrono::system_clock::from_time_t(
                static_cast<std::time_t>(j["ts"].get<double>()));
        }
        if (j.contains("behind") && j["behind"].is_number()) {
            out.behind = j["behind"].get<std::size_t>();
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_update_cache(const fs::path& cache_file,
                        const UpdateCache& entry) {
    std::error_code ec;
    fs::create_directories(cache_file.parent_path(), ec);
    nlohmann::json j;
    j["ts"] = std::chrono::system_clock::to_time_t(entry.timestamp);
    if (entry.behind) j["behind"] = *entry.behind;
    else j["behind"] = nullptr;
    std::ofstream f(cache_file);
    if (!f) return false;
    f << j.dump();
    return static_cast<bool>(f);
}

bool cache_is_fresh(const UpdateCache& entry, std::chrono::seconds max_age) {
    auto age = std::chrono::system_clock::now() - entry.timestamp;
    return age < max_age;
}

// --- Rendering --------------------------------------------------------------

namespace {
constexpr const char* kReset = "\033[0m";
constexpr const char* kGold  = "\033[1;38;2;255;215;0m";
constexpr const char* kAmber = "\033[38;2;255;191;0m";
constexpr const char* kBronze = "\033[38;2;205;127;50m";
constexpr const char* kDim   = "\033[2m";
constexpr const char* kBold  = "\033[1m";
constexpr const char* kYellow = "\033[33m";
}

void render_banner(std::ostream& out, const BannerContext& ctx) {
    out << "\n";

    // Logo (wide terminals only).
    if (ctx.terminal_width >= 95) {
        const char* palette[6] = {kGold, kGold, kAmber, kAmber, kBronze, kBronze};
        for (std::size_t i = 0; i < kHermesLogo.size(); ++i) {
            out << palette[i] << kHermesLogo[i] << kReset << "\n";
        }
        out << "\n";
    }

    // Panel title: version + git state.
    std::string title = format_banner_version_label(ctx.version, ctx.release_date);
    out << kGold << "┌─ " << title << " ─┐" << kReset << "\n";

    // Left column: caduceus + model info.
    for (const auto& l : kHermesCaduceus) out << kBronze << l << kReset << "\n";

    std::string model_short = short_model_name(ctx.model);
    out << kAmber << model_short << kReset;
    if (ctx.context_length) {
        out << kDim << " · " << format_context_length(*ctx.context_length)
            << " context" << kReset;
    }
    out << kDim << " · Nous Research" << kReset << "\n";
    out << kDim << ctx.cwd << kReset << "\n";
    if (!ctx.session_id.empty()) {
        out << kDim << "Session: " << ctx.session_id << kReset << "\n";
    }

    // Tools by toolset.
    out << "\n" << kBold << kAmber << "Available Tools" << kReset << "\n";
    std::map<std::string, std::vector<std::string>> toolsets_dict;
    for (const auto& [name, ts] : ctx.tools) {
        toolsets_dict[display_toolset_name(ts)].push_back(name);
    }
    std::vector<std::string> sorted_toolsets;
    for (const auto& kv : toolsets_dict) sorted_toolsets.push_back(kv.first);
    std::sort(sorted_toolsets.begin(), sorted_toolsets.end());
    std::size_t shown = 0;
    for (const auto& ts : sorted_toolsets) {
        if (shown >= 8) break;
        auto names = toolsets_dict[ts];
        std::sort(names.begin(), names.end());
        out << kDim << ts << ":" << kReset << " " << elide_csv(names, 45) << "\n";
        ++shown;
    }
    if (sorted_toolsets.size() > 8) {
        out << kDim << "(and " << (sorted_toolsets.size() - 8)
            << " more toolsets...)" << kReset << "\n";
    }

    // Skills.
    out << "\n" << kBold << kAmber << "Available Skills" << kReset << "\n";
    auto grouped = group_skills_by_category(ctx.skills);
    if (grouped.empty()) {
        out << kDim << "No skills installed" << kReset << "\n";
    } else {
        auto pick = pick_categories(grouped, 20);
        for (const auto& cat : pick.visible) {
            auto names = grouped[cat];
            std::sort(names.begin(), names.end());
            out << kDim << cat << ":" << kReset << " "
                << summarise_names(names) << "\n";
        }
    }

    // Profile.
    if (!ctx.profile_name.empty() && ctx.profile_name != "default") {
        out << "\n" << kBold << kAmber << "Profile: " << kReset
            << ctx.profile_name << "\n";
    }

    // Summary line.
    std::size_t skill_total = 0;
    for (const auto& kv : grouped) skill_total += kv.second.size();
    out << kDim
        << format_summary_line(ctx.tools.size(), skill_total, ctx.mcp_connected)
        << kReset << "\n";

    if (ctx.behind_commits && *ctx.behind_commits > 0) {
        out << kYellow << kBold
            << format_update_warning(*ctx.behind_commits) << kReset << "\n";
    }

    out << kGold << "└" << std::string(40, '-') << "┘" << kReset << "\n";
}

void print_banner(const BannerContext& ctx) { render_banner(std::cout, ctx); }

}  // namespace hermes::cli::banner
