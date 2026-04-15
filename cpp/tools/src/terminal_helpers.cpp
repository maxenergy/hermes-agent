#include "hermes/tools/terminal_helpers.hpp"
#include "hermes/tools/terminal_tool.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <pwd.h>
#include <random>
#include <regex>
#include <sstream>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace hermes::tools::terminal {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool contains_word(std::string_view hay, std::string_view needle) {
    auto pos = hay.find(needle);
    while (pos != std::string_view::npos) {
        bool lb = pos == 0 ||
                  !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
        auto after = pos + needle.size();
        bool rb = after >= hay.size() ||
                  !std::isalnum(static_cast<unsigned char>(hay[after]));
        if (lb && rb) return true;
        pos = hay.find(needle, pos + 1);
    }
    return false;
}

}  // namespace

// ── Workdir safety ────────────────────────────────────────────────────

std::optional<std::string> validate_workdir_charset(std::string_view wd) {
    if (wd.empty()) return std::nullopt;
    auto safe = [](char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) return true;
        switch (c) {
            case '/': case '_': case '-': case '.': case '~':
            case ' ': case '+': case '@': case '=': case ',':
                return true;
            default: return false;
        }
    };
    for (char c : wd) {
        if (!safe(c)) {
            std::ostringstream os;
            os << "Blocked: workdir contains disallowed character '";
            if (std::isprint(static_cast<unsigned char>(c))) os << c;
            else os << "\\x" << std::hex << static_cast<int>(
                                    static_cast<unsigned char>(c));
            os << "'. Use a simple filesystem path without shell "
                  "metacharacters.";
            return os.str();
        }
    }
    return std::nullopt;
}

// ── Dangerous command scan ────────────────────────────────────────────

DangerReport scan_dangerous_command(std::string_view command) {
    DangerReport r;
    std::string lc = to_lower(command);

    if (std::regex_search(
            lc, std::regex(R"(\brm\b[^\n]*\s+-[rRfF]{1,2}[fF]?\s+/(\s|$))"))) {
        r.level = DangerLevel::Block;
        r.category = "rm-rf-root";
        r.reason = "refuses to run 'rm -rf /' — would wipe the filesystem";
        return r;
    }
    if (std::regex_search(
            lc, std::regex(R"(\brm\b[^\n]*\s+-[rRfF]{2}\s+\$home\b)")) ||
        std::regex_search(
            lc, std::regex(R"(\brm\b[^\n]*\s+-[rRfF]{2}\s+~/?\s)"))) {
        r.level = DangerLevel::Warn;
        r.category = "rm-rf-home";
        r.reason = "recursive delete of $HOME";
        return r;
    }
    if (std::regex_search(lc, std::regex(R"(\bdd\b[^\n]*of=/dev/(sd|hd|nvme))"))) {
        r.level = DangerLevel::Block;
        r.category = "dd-raw-disk";
        r.reason = "writes raw bytes to a block device";
        return r;
    }
    if (std::regex_search(lc, std::regex(R"(\bmkfs(\.[a-z0-9]+)?\s+/dev/)"))) {
        r.level = DangerLevel::Block;
        r.category = "mkfs";
        r.reason = "formats a block device";
        return r;
    }
    if (lc.find(":(){ :|:& };:") != std::string::npos ||
        lc.find(":(){:|:&};:") != std::string::npos) {
        r.level = DangerLevel::Block;
        r.category = "fork-bomb";
        r.reason = "classic fork-bomb pattern";
        return r;
    }
    if (std::regex_search(lc, std::regex(R"(\bchmod\b[^\n]*\s+-R\s+0?777\s+/(\s|$))"))) {
        r.level = DangerLevel::Block;
        r.category = "chmod-777-root";
        r.reason = "chmod -R 777 / is destructive";
        return r;
    }
    if (std::regex_search(
            lc,
            std::regex(R"((curl|wget)\b[^\n]*\|\s*(sudo\s+)?(ba)?sh\b)"))) {
        r.level = DangerLevel::Warn;
        r.category = "curl-pipe-sh";
        r.reason = "piping network download straight into a shell";
        return r;
    }
    if (std::regex_search(
            lc, std::regex(R"(\b(shutdown|halt|reboot|poweroff)\b)"))) {
        r.level = DangerLevel::Warn;
        r.category = "host-power";
        r.reason = "power-state change";
        return r;
    }
    if (std::regex_search(lc, std::regex(R"(\biptables\b\s+-F\b)")) ||
        std::regex_search(lc, std::regex(R"(\bnft\b\s+flush\b)"))) {
        r.level = DangerLevel::Warn;
        r.category = "firewall-flush";
        r.reason = "flushing firewall rules";
        return r;
    }
    if (std::regex_search(
            lc,
            std::regex(
                R"(\bgit\s+push\b[^\n]*--force(-with-lease)?\b[^\n]*\b(main|master)\b)"))) {
        r.level = DangerLevel::Warn;
        r.category = "git-force-push";
        r.reason = "force-push to a protected branch";
        return r;
    }
    return r;
}

// ── Allow / deny evaluation ───────────────────────────────────────────

namespace {
bool glob_match(const std::string& pattern, const std::string& text) {
    std::string re;
    re.reserve(pattern.size() * 2 + 4);
    re.push_back('^');
    for (char c : pattern) {
        switch (c) {
            case '*': re += ".*"; break;
            case '?': re += "."; break;
            case '.': re += "\\."; break;
            case '+': case '(': case ')': case '|': case '^':
            case '$': case '{': case '}': case '[': case ']':
            case '\\':
                re.push_back('\\');
                re.push_back(c);
                break;
            default:
                re.push_back(c);
        }
    }
    re.push_back('$');
    try {
        return std::regex_search(text, std::regex(re));
    } catch (...) {
        return false;
    }
}
}  // namespace

AccessDecision evaluate_access(std::string_view command,
                               const std::vector<std::string>& allow,
                               const std::vector<std::string>& deny) {
    std::string cmd(trim(command));
    for (const auto& p : deny) {
        if (glob_match(p, cmd)) return AccessDecision::Deny;
    }
    if (!allow.empty()) {
        for (const auto& p : allow) {
            if (glob_match(p, cmd)) return AccessDecision::Allow;
        }
        return AccessDecision::Deny;
    }
    return AccessDecision::Unlisted;
}

// ── Env var propagation ───────────────────────────────────────────────

std::unordered_map<std::string, std::string>
filter_env(const std::unordered_map<std::string, std::string>& src,
           const EnvPassthroughPolicy& policy) {
    std::unordered_map<std::string, std::string> out;
    out.reserve(src.size());
    for (const auto& [k, v] : src) {
        if (policy.block.count(k)) continue;
        bool blocked_prefix = false;
        for (const auto& p : policy.block_prefixes) {
            if (starts_with(k, p)) { blocked_prefix = true; break; }
        }
        if (blocked_prefix) continue;
        if (policy.forward_all) { out.emplace(k, v); continue; }
        if (policy.allow.count(k)) { out.emplace(k, v); continue; }
        for (const auto& p : policy.allow_prefixes) {
            if (starts_with(k, p)) {
                out.emplace(k, v);
                break;
            }
        }
    }
    return out;
}

std::vector<std::string> split_pattern_list(std::string_view raw) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= raw.size(); ++i) {
        if (i == raw.size() || raw[i] == ':' || raw[i] == ',' ||
            raw[i] == ';') {
            auto piece = trim(raw.substr(start, i - start));
            if (!piece.empty()) out.emplace_back(piece);
            start = i + 1;
        }
    }
    return out;
}

// ── Command categorization ────────────────────────────────────────────

std::string categorize_command(std::string_view command) {
    auto prog = first_program(command);
    if (prog.empty()) return "shell-internal";
    static const std::unordered_map<std::string, std::string> direct = {
        {"git", "git"}, {"gh", "git"},
        {"make", "build"}, {"cmake", "build"}, {"ninja", "build"},
        {"cargo", "build"}, {"rustc", "build"}, {"go", "build"},
        {"javac", "build"}, {"gcc", "build"}, {"g++", "build"},
        {"clang", "build"}, {"clang++", "build"}, {"tsc", "build"},
        {"apt", "pkg-manager"}, {"apt-get", "pkg-manager"},
        {"dnf", "pkg-manager"}, {"yum", "pkg-manager"},
        {"brew", "pkg-manager"}, {"pacman", "pkg-manager"},
        {"pip", "pkg-manager"}, {"pip3", "pkg-manager"},
        {"uv", "pkg-manager"}, {"npm", "pkg-manager"},
        {"yarn", "pkg-manager"}, {"pnpm", "pkg-manager"},
        {"curl", "network"}, {"wget", "network"}, {"ssh", "network"},
        {"scp", "network"}, {"rsync", "network"}, {"nc", "network"},
        {"docker", "container"}, {"podman", "container"},
        {"kubectl", "container"},
        {"ls", "file"}, {"cp", "file"}, {"mv", "file"}, {"rm", "file"},
        {"mkdir", "file"}, {"rmdir", "file"}, {"touch", "file"},
        {"chmod", "file"}, {"chown", "file"}, {"ln", "file"},
        {"ps", "process"}, {"kill", "process"}, {"pkill", "process"},
        {"top", "process"}, {"htop", "process"}, {"pgrep", "process"},
    };
    auto it = direct.find(prog);
    if (it != direct.end()) return it->second;
    static const std::array<const char*, 8> builtins = {
        "cd", "export", "set", "unset", "source", "alias", "echo", "pwd"};
    for (const char* b : builtins) {
        if (prog == b) return "shell-internal";
    }
    return "other";
}

// ── Interactive mode detection ────────────────────────────────────────

bool needs_pty(std::string_view command) {
    static const std::array<const char*, 14> pty_apps = {
        "vim", "nvim", "vi", "emacs", "nano", "joe", "pico",
        "less", "more", "htop", "top", "man", "fzf", "tmux"};
    auto prog = first_program(command);
    for (const auto& p : pty_apps) {
        if (prog == p) return true;
    }
    return false;
}

// ── Signal forwarding ─────────────────────────────────────────────────

int signal_from_name(std::string_view name) {
    std::string s = to_lower(name);
    if (starts_with(s, "sig")) s.erase(0, 3);
    static const std::unordered_map<std::string, int> map = {
        {"hup", SIGHUP},   {"int", SIGINT},   {"quit", SIGQUIT},
        {"ill", SIGILL},   {"abrt", SIGABRT}, {"fpe", SIGFPE},
        {"kill", SIGKILL}, {"segv", SIGSEGV}, {"pipe", SIGPIPE},
        {"alrm", SIGALRM}, {"term", SIGTERM}, {"usr1", SIGUSR1},
        {"usr2", SIGUSR2}, {"chld", SIGCHLD}, {"cont", SIGCONT},
        {"stop", SIGSTOP}, {"tstp", SIGTSTP}, {"ttin", SIGTTIN},
        {"ttou", SIGTTOU},
    };
    auto it = map.find(s);
    if (it != map.end()) return it->second;
    try {
        size_t pos = 0;
        int n = std::stoi(std::string(name), &pos);
        if (pos == name.size() && n > 0 && n < 64) return n;
    } catch (...) {}
    return -1;
}

std::string signal_to_name(int signal) {
    switch (signal) {
        case SIGHUP:   return "SIGHUP";
        case SIGINT:   return "SIGINT";
        case SIGQUIT:  return "SIGQUIT";
        case SIGILL:   return "SIGILL";
        case SIGABRT:  return "SIGABRT";
        case SIGFPE:   return "SIGFPE";
        case SIGKILL:  return "SIGKILL";
        case SIGSEGV:  return "SIGSEGV";
        case SIGPIPE:  return "SIGPIPE";
        case SIGALRM:  return "SIGALRM";
        case SIGTERM:  return "SIGTERM";
        case SIGUSR1:  return "SIGUSR1";
        case SIGUSR2:  return "SIGUSR2";
        case SIGCHLD:  return "SIGCHLD";
        case SIGCONT:  return "SIGCONT";
        case SIGSTOP:  return "SIGSTOP";
        case SIGTSTP:  return "SIGTSTP";
        case SIGTTIN:  return "SIGTTIN";
        case SIGTTOU:  return "SIGTTOU";
    }
    return "";
}

// ── Retry / timeout hint ──────────────────────────────────────────────

int suggested_retry_timeout(std::string_view command, int original) {
    auto lc = to_lower(command);
    int hint = 0;
    static const std::array<std::pair<const char*, int>, 9> rules = {{
        {"npm install", 600},      {"yarn install", 600},
        {"pnpm install", 600},     {"pip install", 600},
        {"apt install", 600},      {"apt-get install", 600},
        {"cmake --build", 900},    {"cargo build", 900},
        {"docker build", 1200},
    }};
    for (const auto& rule : rules) {
        if (lc.find(rule.first) != std::string::npos) {
            hint = std::max(hint, rule.second);
        }
    }
    if (hint <= original) return 0;
    return hint;
}

// ── Sudo detection ────────────────────────────────────────────────────

bool sudo_requires_password(std::string_view rewritten) {
    std::string lc = to_lower(rewritten);
    if (!contains_word(lc, "sudo")) return false;
    if (std::regex_search(lc, std::regex(R"(\bsudo\b[^\n|;&]*\s-n\b)"))) {
        return false;
    }
    if (lc.find("sudo_askpass=") != std::string::npos) return false;
    return true;
}

// ── Output truncation ─────────────────────────────────────────────────

std::string truncate_output(const std::string& combined, std::size_t max) {
    if (combined.size() <= max) return combined;
    std::size_t head = static_cast<std::size_t>(max * 0.6);
    std::size_t tail = max > head + 80 ? max - head - 80 : 16;
    if (tail < 16) tail = 16;
    std::ostringstream os;
    os << combined.substr(0, head) << "\n...[truncated "
       << (combined.size() - head - tail) << " bytes]...\n"
       << combined.substr(combined.size() - tail);
    return os.str();
}

// ── Watch patterns ────────────────────────────────────────────────────

int first_match(const std::string& text,
                const std::vector<std::string>& patterns) {
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        if (!patterns[i].empty() &&
            text.find(patterns[i]) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ── Backoff ───────────────────────────────────────────────────────────

std::chrono::milliseconds backoff_ms(int attempt, int base_ms, int cap_ms) {
    if (attempt < 0) attempt = 0;
    int exp = base_ms;
    for (int i = 0; i < attempt && exp < cap_ms; ++i) exp *= 2;
    if (exp > cap_ms) exp = cap_ms;
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> jitter(0, base_ms);
    return std::chrono::milliseconds(exp + jitter(rng));
}

// ── PID validation ────────────────────────────────────────────────────

std::optional<std::string> validate_pid(int pid) {
    if (pid <= 0) return "pid must be positive";
    if (pid == 1) return "refusing to signal pid 1 (init)";
    if (pid == 2) return "refusing to signal pid 2 (kthreadd)";
    if (pid >= (1 << 22)) return "pid is out of range";
    if (pid == static_cast<int>(::getpid())) {
        return "refusing to signal the hermes process itself";
    }
    return std::nullopt;
}

// ── First program ─────────────────────────────────────────────────────

std::string first_program(std::string_view command) {
    std::size_t i = 0, n = command.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(command[i]))) {
            ++i;
        }
        if (i >= n) break;
        auto pr = read_shell_token(command, i);
        std::string tok = pr.first;
        std::size_t end = pr.second;
        if (tok.empty()) break;
        if (looks_like_env_assignment(tok)) {
            i = end;
            continue;
        }
        auto slash = tok.rfind('/');
        if (slash != std::string::npos) tok = tok.substr(slash + 1);
        return tok;
    }
    return "";
}

// ── expand_user ───────────────────────────────────────────────────────

std::string expand_user(std::string_view path) {
    if (path.empty() || path.front() != '~') return std::string(path);
    auto slash = path.find('/');
    std::string user;
    std::string rest;
    if (slash == std::string_view::npos) {
        user = std::string(path.substr(1));
    } else {
        user = std::string(path.substr(1, slash - 1));
        rest = std::string(path.substr(slash));
    }
    std::string home;
    if (user.empty()) {
        const char* h = ::getenv("HOME");
        if (h && *h) home = h;
        else {
            struct passwd* pw = ::getpwuid(::getuid());
            if (pw && pw->pw_dir) home = pw->pw_dir;
        }
    } else {
        struct passwd* pw = ::getpwnam(user.c_str());
        if (pw && pw->pw_dir) home = pw->pw_dir;
    }
    if (home.empty()) return std::string(path);
    return home + rest;
}

// ── Strip leading env ─────────────────────────────────────────────────

StripEnvResult strip_leading_env(std::string_view command) {
    StripEnvResult r;
    std::size_t i = 0, n = command.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(command[i]))) {
            ++i;
        }
        std::size_t before = i;
        auto pr = read_shell_token(command, i);
        std::string tok = pr.first;
        std::size_t end = pr.second;
        if (tok.empty()) break;
        if (!looks_like_env_assignment(tok)) {
            i = before;
            break;
        }
        auto eq = tok.find('=');
        std::string name = tok.substr(0, eq);
        std::string value = tok.substr(eq + 1);
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        r.env.emplace_back(std::move(name), std::move(value));
        i = end;
    }
    while (i < n && std::isspace(static_cast<unsigned char>(command[i]))) ++i;
    r.remaining = std::string(command.substr(i));
    return r;
}

}  // namespace hermes::tools::terminal
