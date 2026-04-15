// Implementation of agent/display.py's portable surface — see display.hpp.
#include "hermes/agent/display.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::agent::display {

namespace {

// --- ANSI palette (dark-terminal fallback used by the C++ renderer) --------
constexpr const char* kDiffDim   = "\033[38;2;150;150;150m";
constexpr const char* kDiffFile  = "\033[38;2;180;160;255m";
constexpr const char* kDiffHunk  = "\033[38;2;120;120;140m";
constexpr const char* kDiffMinus = "\033[38;2;255;255;255;48;2;120;20;20m";
constexpr const char* kDiffPlus  = "\033[38;2;255;255;255;48;2;20;90;20m";

constexpr const char* kBold     = "\033[1m";
constexpr const char* kYellow   = "\033[33m";
constexpr const char* kDimAnsi  = "\033[2m";

constexpr int kBarWidth = 20;
constexpr const char* kBarFilled = "\xe2\x96\xb0";  // ▰
constexpr const char* kBarEmpty  = "\xe2\x96\xb1";  // ▱

int g_tool_preview_max_len = 0;  // 0 = unlimited (matches Python module default)

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string lowercase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Truncate `s` to `n` chars with trailing "...". When max_len == 0 (unlimited)
// returns the input untouched (matches the Python `_trunc` closure semantics).
std::string trunc(std::string_view s, int n) {
    if (g_tool_preview_max_len == 0) return std::string(s);
    if (n <= 3 || static_cast<int>(s.size()) <= n) return std::string(s);
    return std::string(s.substr(0, static_cast<size_t>(n - 3))) + "...";
}

// Reverse-truncate paths: keep the tail "..." + last (n-3) chars.
std::string trunc_path(std::string_view p, int n) {
    if (g_tool_preview_max_len == 0) return std::string(p);
    if (n <= 3 || static_cast<int>(p.size()) <= n) return std::string(p);
    return "..." + std::string(p.substr(p.size() - (n - 3)));
}

// Pull an arbitrary scalar key out of an object as a printable string.
std::string get_str(const nlohmann::json& obj, const char* key, const std::string& fallback = "") {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

bool has_key(const nlohmann::json& obj, const char* key) {
    return obj.is_object() && obj.find(key) != obj.end();
}

// Strip the URL scheme then take the leftmost host component.
std::string url_domain(std::string_view url) {
    std::string s(url);
    auto strip = [&](std::string_view prefix) {
        if (starts_with(s, prefix)) s.erase(0, prefix.size());
    };
    strip("https://");
    strip("http://");
    auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    return s;
}

}  // namespace

// ============================================================================
// Tool preview length
// ============================================================================
void set_tool_preview_max_len(int n) noexcept {
    g_tool_preview_max_len = n > 0 ? n : 0;
}

int get_tool_preview_max_len() noexcept {
    return g_tool_preview_max_len;
}

std::string oneline(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_ws = false;
    bool started = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            in_ws = true;
        } else {
            if (in_ws && started) out.push_back(' ');
            out.push_back(c);
            started = true;
            in_ws = false;
        }
    }
    return out;
}

std::optional<std::string> build_tool_preview(
    const std::string& tool_name,
    const nlohmann::json& args,
    int max_len) {
    if (max_len < 0) max_len = g_tool_preview_max_len;
    if (!args.is_object() || args.empty()) return std::nullopt;

    // process: action sid "data" timeout
    if (tool_name == "process") {
        std::string action = get_str(args, "action");
        std::string sid    = get_str(args, "session_id");
        std::string data   = get_str(args, "data");
        std::vector<std::string> parts;
        if (!action.empty()) parts.push_back(action);
        if (!sid.empty())    parts.push_back(sid.substr(0, 16));
        if (!data.empty()) {
            std::string snippet = data.size() > 20 ? data.substr(0, 20) : data;
            parts.push_back("\"" + oneline(snippet) + "\"");
        }
        if (action == "wait" && has_key(args, "timeout")) {
            parts.push_back(args["timeout"].dump() + "s");
        }
        if (parts.empty()) return std::nullopt;
        std::string joined = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) joined += " " + parts[i];
        return joined;
    }

    // todo: handle three states (None / merge / fresh plan)
    if (tool_name == "todo") {
        bool has_todos = has_key(args, "todos") && !args["todos"].is_null();
        bool merge = has_key(args, "merge") && args["merge"].is_boolean() && args["merge"].get<bool>();
        if (!has_todos) return std::string("reading task list");
        size_t n = args["todos"].is_array() ? args["todos"].size() : 0;
        if (merge) return "updating " + std::to_string(n) + " task(s)";
        return "planning " + std::to_string(n) + " task(s)";
    }

    if (tool_name == "session_search") {
        std::string q = oneline(get_str(args, "query"));
        std::string head = q.size() > 25 ? q.substr(0, 25) + "..." : q;
        return std::string("recall: \"") + head + "\"";
    }

    if (tool_name == "memory") {
        std::string action = get_str(args, "action");
        std::string target = get_str(args, "target");
        if (action == "add") {
            std::string c = oneline(get_str(args, "content"));
            std::string head = c.size() > 25 ? c.substr(0, 25) + "..." : c;
            return "+" + target + ": \"" + head + "\"";
        }
        if (action == "replace") {
            std::string o = oneline(get_str(args, "old_text"));
            return "~" + target + ": \"" + (o.size() > 20 ? o.substr(0, 20) : o) + "\"";
        }
        if (action == "remove") {
            std::string o = oneline(get_str(args, "old_text"));
            return "-" + target + ": \"" + (o.size() > 20 ? o.substr(0, 20) : o) + "\"";
        }
        return action;
    }

    if (tool_name == "send_message") {
        std::string target = get_str(args, "target", "?");
        std::string msg    = oneline(get_str(args, "message"));
        if (msg.size() > 20) msg = msg.substr(0, 17) + "...";
        return std::string("to ") + target + ": \"" + msg + "\"";
    }

    if (starts_with(tool_name, "rl_")) {
        if (tool_name == "rl_list_environments")     return std::string("listing envs");
        if (tool_name == "rl_select_environment")    return get_str(args, "name");
        if (tool_name == "rl_get_current_config")    return std::string("reading config");
        if (tool_name == "rl_edit_config")
            return get_str(args, "field") + "=" + get_str(args, "value");
        if (tool_name == "rl_start_training")        return std::string("starting");
        if (tool_name == "rl_check_status")          return get_str(args, "run_id").substr(0, 16);
        if (tool_name == "rl_stop_training")         return std::string("stopping ") + get_str(args, "run_id").substr(0, 16);
        if (tool_name == "rl_get_results")           return get_str(args, "run_id").substr(0, 16);
        if (tool_name == "rl_list_runs")             return std::string("listing runs");
        if (tool_name == "rl_test_inference") {
            int steps = has_key(args, "num_steps") && args["num_steps"].is_number_integer()
                ? args["num_steps"].get<int>() : 3;
            return std::to_string(steps) + " steps";
        }
        return std::nullopt;
    }

    // primary_args lookup
    static const std::unordered_map<std::string, std::string> kPrimary = {
        {"terminal", "command"}, {"web_search", "query"}, {"web_extract", "urls"},
        {"read_file", "path"}, {"write_file", "path"}, {"patch", "path"},
        {"search_files", "pattern"}, {"browser_navigate", "url"},
        {"browser_click", "ref"}, {"browser_type", "text"},
        {"image_generate", "prompt"}, {"text_to_speech", "text"},
        {"vision_analyze", "question"}, {"mixture_of_agents", "user_prompt"},
        {"skill_view", "name"}, {"skills_list", "category"},
        {"cronjob", "action"}, {"execute_code", "code"},
        {"delegate_task", "goal"}, {"clarify", "question"},
        {"skill_manage", "name"},
    };

    std::string key;
    auto it = kPrimary.find(tool_name);
    if (it != kPrimary.end()) key = it->second;
    if (key.empty() || !has_key(args, key.c_str())) {
        // Fallback: walk a list of common keys.
        static const char* kFallbacks[] = {"query","text","command","path","name","prompt","code","goal"};
        for (const char* fk : kFallbacks) {
            if (has_key(args, fk)) { key = fk; break; }
        }
    }
    if (key.empty() || !has_key(args, key.c_str())) return std::nullopt;

    const auto& v = args[key];
    std::string preview;
    if (v.is_array()) {
        if (!v.empty()) {
            const auto& head = v[0];
            preview = head.is_string() ? head.get<std::string>() : head.dump();
        }
    } else if (v.is_string()) {
        preview = v.get<std::string>();
    } else {
        preview = v.dump();
    }
    preview = oneline(preview);
    if (preview.empty()) return std::nullopt;
    if (max_len > 3 && static_cast<int>(preview.size()) > max_len) {
        preview = preview.substr(0, max_len - 3) + "...";
    }
    return preview;
}

// ============================================================================
// Tool failure detection
// ============================================================================
FailureInfo detect_tool_failure(
    const std::string& tool_name,
    const std::optional<std::string>& result) {
    FailureInfo info;
    if (!result.has_value()) return info;
    const std::string& r = *result;

    if (tool_name == "terminal") {
        try {
            auto data = nlohmann::json::parse(r);
            if (data.is_object() && data.contains("exit_code") && data["exit_code"].is_number_integer()) {
                int code = data["exit_code"].get<int>();
                if (code != 0) {
                    info.is_failure = true;
                    info.suffix = " [exit " + std::to_string(code) + "]";
                }
            }
        } catch (...) {
            // ignore — terminal results aren't always JSON
        }
        return info;
    }

    if (tool_name == "memory") {
        try {
            auto data = nlohmann::json::parse(r);
            if (data.is_object() && data.contains("success")
                && data["success"].is_boolean() && !data["success"].get<bool>()) {
                std::string err = data.value("error", std::string{});
                if (err.find("exceed the limit") != std::string::npos) {
                    info.is_failure = true;
                    info.suffix = " [full]";
                    return info;
                }
            }
        } catch (...) {}
    }

    std::string head = r.size() > 500 ? r.substr(0, 500) : r;
    std::string lower = lowercase(head);
    if (lower.find("\"error\"") != std::string::npos
        || lower.find("\"failed\"") != std::string::npos
        || starts_with(r, "Error")) {
        info.is_failure = true;
        info.suffix = " [error]";
    }
    return info;
}

// ============================================================================
// Cute tool message
// ============================================================================
namespace {

std::string format_duration(double sec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", sec);
    return buf;
}

std::string wrap_with_failure(const std::string& line, const FailureInfo& info) {
    if (!info.is_failure) return line;
    return line + info.suffix;
}

// Canonical "┊ {emoji} {verb:9} {body}  {dur}" formatter.
std::string row(const std::string& emoji, const std::string& verb, const std::string& body,
                const std::string& dur) {
    // Verb column is 9 chars wide.
    std::string padded = verb;
    if (padded.size() < 9) padded.append(9 - padded.size(), ' ');
    return std::string("┊ ") + emoji + " " + padded + " " + body + "  " + dur;
}

}  // namespace

std::string get_cute_tool_message(
    const std::string& tool_name,
    const nlohmann::json& args,
    double duration_seconds,
    const std::optional<std::string>& result) {
    std::string dur = format_duration(duration_seconds);
    FailureInfo failure = detect_tool_failure(tool_name, result);
    auto wrap = [&](std::string line) { return wrap_with_failure(std::move(line), failure); };

    if (tool_name == "web_search") {
        return wrap(row("🔍", "search", trunc(get_str(args, "query"), 42), dur));
    }
    if (tool_name == "web_extract") {
        if (args.is_object() && args.contains("urls") && args["urls"].is_array() && !args["urls"].empty()) {
            const auto& urls = args["urls"];
            std::string url = urls[0].is_string() ? urls[0].get<std::string>() : urls[0].dump();
            std::string extra = urls.size() > 1 ? " +" + std::to_string(urls.size() - 1) : "";
            return wrap(row("📄", "fetch", trunc(url_domain(url), 35) + extra, dur));
        }
        return wrap(row("📄", "fetch", "pages", dur));
    }
    if (tool_name == "web_crawl") {
        return wrap(row("🕸️ ", "crawl", trunc(url_domain(get_str(args, "url")), 35), dur));
    }
    if (tool_name == "terminal") {
        return wrap(row("💻", "$", trunc(get_str(args, "command"), 42), dur));
    }
    if (tool_name == "process") {
        std::string action = get_str(args, "action", "?");
        std::string sid    = get_str(args, "session_id").substr(0, 12);
        static const std::unordered_map<std::string, std::string> kLabels = {
            {"list", "ls processes"}, {"poll", "poll "},   {"log", "log "},
            {"wait", "wait "},        {"kill", "kill "},   {"write", "write "},
            {"submit", "submit "},
        };
        auto it = kLabels.find(action);
        std::string label;
        if (it == kLabels.end()) label = action + " " + sid;
        else if (action == "list") label = it->second;
        else label = it->second + sid;
        return wrap(row("⚙️ ", "proc", label, dur));
    }
    if (tool_name == "read_file")  return wrap(row("📖", "read",  trunc_path(get_str(args, "path"), 35), dur));
    if (tool_name == "write_file") return wrap(row("✍️ ", "write", trunc_path(get_str(args, "path"), 35), dur));
    if (tool_name == "patch")      return wrap(row("🔧", "patch", trunc_path(get_str(args, "path"), 35), dur));
    if (tool_name == "search_files") {
        std::string pattern = trunc(get_str(args, "pattern"), 35);
        std::string target = get_str(args, "target", "content");
        std::string verb = (target == "files") ? "find" : "grep";
        return wrap(row("🔎", verb, pattern, dur));
    }
    if (tool_name == "browser_navigate") {
        return wrap(row("🌐", "navigate", trunc(url_domain(get_str(args, "url")), 35), dur));
    }
    if (tool_name == "browser_snapshot") {
        bool full = has_key(args, "full") && args["full"].is_boolean() && args["full"].get<bool>();
        return wrap(row("📸", "snapshot", full ? "full" : "compact", dur));
    }
    if (tool_name == "browser_click") return wrap(row("👆", "click", get_str(args, "ref", "?"), dur));
    if (tool_name == "browser_type")  return wrap(row("⌨️ ", "type", "\"" + trunc(get_str(args, "text"), 30) + "\"", dur));
    if (tool_name == "browser_scroll") {
        std::string d = get_str(args, "direction", "down");
        std::string arrow = "↓";
        if (d == "up") arrow = "↑";
        else if (d == "right") arrow = "→";
        else if (d == "left")  arrow = "←";
        return wrap(row(arrow, "scroll", d, dur));
    }
    if (tool_name == "browser_back")        return wrap(std::string("┊ ◀️  back      ") + dur);
    if (tool_name == "browser_press")       return wrap(row("⌨️ ", "press", get_str(args, "key", "?"), dur));
    if (tool_name == "browser_get_images")  return wrap(row("🖼️ ", "images", "extracting", dur));
    if (tool_name == "browser_vision")      return wrap(row("👁️ ", "vision", "analyzing page", dur));
    if (tool_name == "todo") {
        bool has_todos = has_key(args, "todos") && !args["todos"].is_null();
        bool merge = has_key(args, "merge") && args["merge"].is_boolean() && args["merge"].get<bool>();
        if (!has_todos) return wrap(row("📋", "plan", "reading tasks", dur));
        size_t n = args["todos"].is_array() ? args["todos"].size() : 0;
        if (merge) return wrap(row("📋", "plan", "update " + std::to_string(n) + " task(s)", dur));
        return wrap(row("📋", "plan", std::to_string(n) + " task(s)", dur));
    }
    if (tool_name == "session_search") {
        return wrap(row("🔍", "recall", "\"" + trunc(get_str(args, "query"), 35) + "\"", dur));
    }
    if (tool_name == "memory") {
        std::string action = get_str(args, "action", "?");
        std::string target = get_str(args, "target");
        if (action == "add")
            return wrap(row("🧠", "memory", "+" + target + ": \"" + trunc(get_str(args, "content"), 30) + "\"", dur));
        if (action == "replace")
            return wrap(row("🧠", "memory", "~" + target + ": \"" + trunc(get_str(args, "old_text"), 20) + "\"", dur));
        if (action == "remove")
            return wrap(row("🧠", "memory", "-" + target + ": \"" + trunc(get_str(args, "old_text"), 20) + "\"", dur));
        return wrap(row("🧠", "memory", action, dur));
    }
    if (tool_name == "skills_list")  return wrap(row("📚", "skills", "list " + get_str(args, "category", "all"), dur));
    if (tool_name == "skill_view")   return wrap(row("📚", "skill", trunc(get_str(args, "name"), 30), dur));
    if (tool_name == "image_generate")
        return wrap(row("🎨", "create", trunc(get_str(args, "prompt"), 35), dur));
    if (tool_name == "text_to_speech")
        return wrap(row("🔊", "speak", trunc(get_str(args, "text"), 30), dur));
    if (tool_name == "vision_analyze")
        return wrap(row("👁️ ", "vision", trunc(get_str(args, "question"), 30), dur));
    if (tool_name == "mixture_of_agents")
        return wrap(row("🧠", "reason", trunc(get_str(args, "user_prompt"), 30), dur));
    if (tool_name == "send_message") {
        return wrap(row("📨", "send", get_str(args, "target", "?")
            + ": \"" + trunc(get_str(args, "message"), 25) + "\"", dur));
    }
    if (tool_name == "cronjob") {
        std::string action = get_str(args, "action", "?");
        if (action == "create") {
            std::string label = get_str(args, "name");
            if (label.empty() && has_key(args, "skills") && args["skills"].is_array() && !args["skills"].empty()) {
                label = args["skills"][0].is_string() ? args["skills"][0].get<std::string>()
                                                     : args["skills"][0].dump();
            }
            if (label.empty()) label = get_str(args, "skill");
            if (label.empty()) label = get_str(args, "prompt", "task");
            return wrap(row("⏰", "cron", "create " + trunc(label, 24), dur));
        }
        if (action == "list") return wrap(row("⏰", "cron", "listing", dur));
        return wrap(row("⏰", "cron", action + " " + get_str(args, "job_id"), dur));
    }
    if (starts_with(tool_name, "rl_")) {
        std::string body = tool_name.substr(3);  // strip "rl_"
        if (tool_name == "rl_list_environments")    body = "list envs";
        else if (tool_name == "rl_select_environment") body = "select " + get_str(args, "name");
        else if (tool_name == "rl_get_current_config") body = "get config";
        else if (tool_name == "rl_edit_config")     body = "set " + get_str(args, "field", "?");
        else if (tool_name == "rl_start_training")  body = "start training";
        else if (tool_name == "rl_check_status")    body = "status " + get_str(args, "run_id", "?").substr(0, 12);
        else if (tool_name == "rl_stop_training")   body = "stop "   + get_str(args, "run_id", "?").substr(0, 12);
        else if (tool_name == "rl_get_results")     body = "results "+ get_str(args, "run_id", "?").substr(0, 12);
        else if (tool_name == "rl_list_runs")       body = "list runs";
        else if (tool_name == "rl_test_inference")  body = "test inference";
        return wrap(row("🧪", "rl", body, dur));
    }
    if (tool_name == "execute_code") {
        std::string code = get_str(args, "code");
        std::string trimmed = trim(code);
        std::string first_line;
        auto nl = trimmed.find('\n');
        first_line = (nl == std::string::npos) ? trimmed : trimmed.substr(0, nl);
        return wrap(row("🐍", "exec", trunc(first_line, 35), dur));
    }
    if (tool_name == "delegate_task") {
        if (has_key(args, "tasks") && args["tasks"].is_array()) {
            return wrap(row("🔀", "delegate",
                std::to_string(args["tasks"].size()) + " parallel tasks", dur));
        }
        return wrap(row("🔀", "delegate", trunc(get_str(args, "goal"), 35), dur));
    }

    auto preview = build_tool_preview(tool_name, args).value_or("");
    std::string verb = tool_name.size() > 9 ? tool_name.substr(0, 9) : tool_name;
    return wrap(row("⚡", verb, trunc(preview, 35), dur));
}

// ============================================================================
// Local edit snapshot
// ============================================================================
namespace {

std::optional<std::string> read_text_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::path resolve_path(std::string_view raw) {
    std::string s(raw);
    if (!s.empty() && s.front() == '~') {
        const char* home = std::getenv("HOME");
        if (home) s = std::string(home) + s.substr(1);
    }
    std::filesystem::path p(s);
    if (p.is_absolute()) return p;
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return p;
    return cwd / p;
}

std::string display_diff_path(const std::filesystem::path& p) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(p, ec);
    if (ec) return p.string();
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return resolved.string();
    auto cwd_resolved = std::filesystem::weakly_canonical(cwd, ec);
    if (ec) return resolved.string();
    auto rel = std::filesystem::relative(resolved, cwd_resolved, ec);
    if (ec || rel.empty() || rel.string().rfind("..", 0) == 0) return resolved.string();
    return rel.string();
}

std::vector<std::string> split_lines_keep_endings(const std::string& s) {
    std::vector<std::string> lines;
    if (s.empty()) return lines;
    std::string cur;
    for (char c : s) {
        cur.push_back(c);
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

// Compact LCS-based unified diff used by the snapshot diff renderer.
//
// Not an exact byte-for-byte match for Python's difflib (which uses
// SequenceMatcher's "junk" heuristics + grouping), but produces a unified
// diff with `--- a/<file>` / `+++ b/<file>` / `@@ -a,b +c,d @@` headers
// that downstream code (split_unified_diff_sections / render_inline_*)
// can consume.  Sufficient for inline diff previews.
std::string make_unified_diff(
    const std::vector<std::string>& a,
    const std::vector<std::string>& b,
    const std::string& from_label,
    const std::string& to_label) {
    const size_t n = a.size();
    const size_t m = b.size();
    // LCS table
    std::vector<std::vector<size_t>> dp(n + 1, std::vector<size_t>(m + 1, 0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < m; ++j) {
            if (a[i] == b[j]) dp[i + 1][j + 1] = dp[i][j] + 1;
            else dp[i + 1][j + 1] = std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }
    // Build edit script: list of {tag, line}, tags '=', '-', '+'.
    std::vector<std::pair<char, std::string>> ops;
    {
        size_t i = n, j = m;
        while (i > 0 && j > 0) {
            if (a[i - 1] == b[j - 1]) {
                ops.push_back({' ', a[i - 1]});
                --i; --j;
            } else if (dp[i - 1][j] >= dp[i][j - 1]) {
                ops.push_back({'-', a[i - 1]});
                --i;
            } else {
                ops.push_back({'+', b[j - 1]});
                --j;
            }
        }
        while (i > 0) { ops.push_back({'-', a[--i]}); }
        while (j > 0) { ops.push_back({'+', b[--j]}); }
        std::reverse(ops.begin(), ops.end());
    }

    // Quick check — any change at all?
    bool changed = false;
    for (auto& op : ops) {
        if (op.first != ' ') { changed = true; break; }
    }
    if (!changed) return "";

    std::ostringstream out;
    out << "--- " << from_label << "\n";
    out << "+++ " << to_label << "\n";

    // Single hunk covering the entire file (matches what difflib produces for
    // small files when context is unbounded).  Compute counts first.
    size_t a_count = 0, b_count = 0;
    for (auto& op : ops) {
        if (op.first == ' ') { ++a_count; ++b_count; }
        else if (op.first == '-') ++a_count;
        else if (op.first == '+') ++b_count;
    }
    out << "@@ -" << (a_count == 0 ? 0 : 1) << "," << a_count
        << " +" << (b_count == 0 ? 0 : 1) << "," << b_count << " @@\n";
    for (auto& op : ops) {
        std::string line = op.second;
        bool needs_nl = line.empty() || line.back() != '\n';
        out << op.first << line;
        if (needs_nl) out << "\n";
    }
    return out.str();
}

}  // namespace

std::vector<std::filesystem::path> resolve_local_edit_paths(
    const std::string& tool_name,
    const nlohmann::json& function_args) {
    if (!function_args.is_object()) return {};
    if (tool_name == "write_file" || tool_name == "patch") {
        std::string path = get_str(function_args, "path");
        if (path.empty()) return {};
        return {resolve_path(path)};
    }
    // skill_manage paths are resolved through the Python skill manager —
    // the C++ port intentionally returns nothing and lets callers handle it.
    return {};
}

std::optional<LocalEditSnapshot> capture_local_edit_snapshot(
    const std::string& tool_name,
    const nlohmann::json& function_args) {
    auto paths = resolve_local_edit_paths(tool_name, function_args);
    if (paths.empty()) return std::nullopt;
    LocalEditSnapshot snap;
    snap.paths = paths;
    for (auto& p : paths) {
        snap.before[p.string()] = read_text_file(p);
    }
    return snap;
}

bool result_succeeded(const std::optional<std::string>& result) {
    if (!result.has_value() || result->empty()) return false;
    nlohmann::json data;
    try { data = nlohmann::json::parse(*result); }
    catch (...) { return false; }
    if (!data.is_object()) return false;
    if (data.contains("error") && !data["error"].is_null()) {
        // truthy error -> failure
        if (data["error"].is_string()) return data["error"].get<std::string>().empty();
        return false;
    }
    if (data.contains("success")) return static_cast<bool>(data["success"]);
    return true;
}

std::optional<std::string> diff_from_snapshot(
    const std::optional<LocalEditSnapshot>& snap_opt) {
    if (!snap_opt.has_value()) return std::nullopt;
    const auto& snap = *snap_opt;
    std::string combined;
    for (const auto& path : snap.paths) {
        auto before_it = snap.before.find(path.string());
        std::optional<std::string> before = (before_it != snap.before.end())
            ? before_it->second : std::nullopt;
        auto after = read_text_file(path);
        if (before == after) continue;
        std::string disp = display_diff_path(path);
        auto a_lines = before.has_value() ? split_lines_keep_endings(*before) : std::vector<std::string>{};
        auto b_lines = after .has_value() ? split_lines_keep_endings(*after)  : std::vector<std::string>{};
        auto chunk = make_unified_diff(a_lines, b_lines, "a/" + disp, "b/" + disp);
        if (!chunk.empty()) {
            if (chunk.back() != '\n') chunk.push_back('\n');
            combined += chunk;
        }
    }
    if (combined.empty()) return std::nullopt;
    return combined;
}

std::optional<std::string> extract_edit_diff(
    const std::string& tool_name,
    const std::optional<std::string>& result,
    const nlohmann::json& /*function_args*/,
    const std::optional<LocalEditSnapshot>& snapshot) {
    if (tool_name == "patch" && result.has_value()) {
        try {
            auto data = nlohmann::json::parse(*result);
            if (data.is_object() && data.contains("diff") && data["diff"].is_string()) {
                std::string d = data["diff"].get<std::string>();
                if (!trim(d).empty()) return d;
            }
        } catch (...) {}
    }
    if (tool_name != "write_file" && tool_name != "patch" && tool_name != "skill_manage") {
        return std::nullopt;
    }
    if (!result_succeeded(result)) return std::nullopt;
    return diff_from_snapshot(snapshot);
}

std::vector<std::string> split_unified_diff_sections(const std::string& diff) {
    std::vector<std::vector<std::string>> sections;
    std::vector<std::string> current;
    auto flush = [&]() {
        if (!current.empty()) {
            sections.push_back(std::move(current));
            current.clear();
        }
    };
    std::string line;
    std::istringstream iss(diff);
    while (std::getline(iss, line)) {
        if (starts_with(line, "--- ") && !current.empty()) {
            flush();
        }
        current.push_back(line);
    }
    flush();
    std::vector<std::string> joined;
    for (auto& sec : sections) {
        if (sec.empty()) continue;
        std::string out;
        for (size_t i = 0; i < sec.size(); ++i) {
            if (i) out.push_back('\n');
            out += sec[i];
        }
        joined.push_back(std::move(out));
    }
    return joined;
}

std::vector<std::string> render_inline_unified_diff(const std::string& diff) {
    std::vector<std::string> rendered;
    std::optional<std::string> from_file;
    std::optional<std::string> to_file;
    std::istringstream iss(diff);
    std::string raw;
    while (std::getline(iss, raw)) {
        if (starts_with(raw, "--- ")) {
            from_file = trim(raw.substr(4));
            continue;
        }
        if (starts_with(raw, "+++ ")) {
            to_file = trim(raw.substr(4));
            if (from_file || to_file) {
                std::string a = from_file.value_or("a/?");
                std::string b = to_file.value_or("b/?");
                rendered.push_back(std::string(kDiffFile) + a + " → " + b + kAnsiReset);
            }
            continue;
        }
        if (starts_with(raw, "@@")) {
            rendered.push_back(std::string(kDiffHunk) + raw + kAnsiReset);
            continue;
        }
        if (!raw.empty() && raw[0] == '-') {
            rendered.push_back(std::string(kDiffMinus) + raw + kAnsiReset);
            continue;
        }
        if (!raw.empty() && raw[0] == '+') {
            rendered.push_back(std::string(kDiffPlus) + raw + kAnsiReset);
            continue;
        }
        if (!raw.empty() && raw[0] == ' ') {
            rendered.push_back(std::string(kDiffDim) + raw + kAnsiReset);
            continue;
        }
        if (!raw.empty()) rendered.push_back(raw);
    }
    return rendered;
}

std::vector<std::string> summarize_rendered_diff_sections(
    const std::string& diff,
    int max_files,
    int max_lines) {
    auto sections = split_unified_diff_sections(diff);
    std::vector<std::string> rendered;
    int omitted_files = 0;
    int omitted_lines = 0;

    for (size_t idx = 0; idx < sections.size(); ++idx) {
        if (static_cast<int>(idx) >= max_files) {
            ++omitted_files;
            omitted_lines += static_cast<int>(render_inline_unified_diff(sections[idx]).size());
            continue;
        }
        auto section_lines = render_inline_unified_diff(sections[idx]);
        int remaining = max_lines - static_cast<int>(rendered.size());
        if (remaining <= 0) {
            omitted_lines += static_cast<int>(section_lines.size());
            ++omitted_files;
            continue;
        }
        if (static_cast<int>(section_lines.size()) <= remaining) {
            rendered.insert(rendered.end(), section_lines.begin(), section_lines.end());
            continue;
        }
        rendered.insert(rendered.end(), section_lines.begin(), section_lines.begin() + remaining);
        omitted_lines += static_cast<int>(section_lines.size()) - remaining;
        // remaining sections — count their lines + bump file counter
        omitted_files += 1 + static_cast<int>(sections.size() - idx - 1);
        for (size_t k = idx + 1; k < sections.size(); ++k) {
            omitted_lines += static_cast<int>(render_inline_unified_diff(sections[k]).size());
        }
        break;
    }

    if (omitted_files || omitted_lines) {
        std::string summary = "… omitted " + std::to_string(omitted_lines) + " diff line(s)";
        if (omitted_files) {
            summary += " across " + std::to_string(omitted_files) + " additional file(s)/section(s)";
        }
        rendered.push_back(std::string(kDiffHunk) + summary + kAnsiReset);
    }
    return rendered;
}

// ============================================================================
// Context pressure
// ============================================================================
namespace {

std::string repeat(const char* utf8, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) s += utf8;
    return s;
}

}  // namespace

std::string format_context_pressure(
    double compaction_progress,
    long long threshold_tokens,
    double threshold_percent,
    bool compression_enabled) {
    int pct_int = std::min(static_cast<int>(compaction_progress * 100), 100);
    int filled = std::min(static_cast<int>(compaction_progress * kBarWidth), kBarWidth);
    if (filled < 0) filled = 0;
    if (pct_int < 0) pct_int = 0;
    std::string bar = repeat(kBarFilled, filled) + repeat(kBarEmpty, kBarWidth - filled);

    std::string threshold_label;
    if (threshold_tokens >= 1000) {
        threshold_label = std::to_string(threshold_tokens / 1000) + "k";
    } else {
        threshold_label = std::to_string(threshold_tokens);
    }
    int threshold_pct_int = static_cast<int>(threshold_percent * 100);

    std::string color = std::string(kBold) + kYellow;
    std::string icon = "⚠";
    std::string hint = compression_enabled ? "compaction approaching" : "no auto-compaction";

    return std::string("  ") + color + icon + " context " + bar + " "
        + std::to_string(pct_int) + "% to compaction" + kAnsiReset
        + "  " + kDimAnsi + threshold_label + " threshold ("
        + std::to_string(threshold_pct_int) + "%) · " + hint + kAnsiReset;
}

std::string format_context_pressure_gateway(
    double compaction_progress,
    double threshold_percent,
    bool compression_enabled) {
    int pct_int = std::min(static_cast<int>(compaction_progress * 100), 100);
    int filled = std::min(static_cast<int>(compaction_progress * kBarWidth), kBarWidth);
    if (filled < 0) filled = 0;
    if (pct_int < 0) pct_int = 0;
    std::string bar = repeat(kBarFilled, filled) + repeat(kBarEmpty, kBarWidth - filled);
    int threshold_pct_int = static_cast<int>(threshold_percent * 100);

    std::string icon = "⚠️";
    std::string hint = compression_enabled
        ? "Context compaction approaching (threshold: " + std::to_string(threshold_pct_int) + "% of window)."
        : "Auto-compaction is disabled — context may be truncated.";

    return std::string(icon) + " context " + bar + " " + std::to_string(pct_int)
         + "% to compaction. " + hint;
}

// ============================================================================
// Spinner data tables
// ============================================================================
const std::vector<std::string>& kawaii_waiting_faces() {
    static const std::vector<std::string> kFaces = {
        "(｡◕‿◕｡)", "(◕‿◕✿)", "٩(◕‿◕｡)۶", "(✿◠‿◠)", "( ˘▽˘)っ",
        "♪(´ε` )", "(◕ᴗ◕✿)", "ヾ(＾∇＾)", "(≧◡≦)", "(★ω★)",
    };
    return kFaces;
}

const std::vector<std::string>& kawaii_thinking_faces() {
    static const std::vector<std::string> kFaces = {
        "(｡•́︿•̀｡)", "(◔_◔)", "(¬‿¬)", "( •_•)>⌐■-■", "(⌐■_■)",
        "(´･_･`)", "◉_◉", "(°ロ°)", "( ˘⌣˘)♡", "ヽ(>∀<☆)☆",
        "٩(๑❛ᴗ❛๑)۶", "(⊙_⊙)", "(¬_¬)", "( ͡° ͜ʖ ͡°)", "ಠ_ಠ",
    };
    return kFaces;
}

const std::vector<std::string>& thinking_verbs() {
    static const std::vector<std::string> kVerbs = {
        "pondering", "contemplating", "musing", "cogitating", "ruminating",
        "deliberating", "mulling", "reflecting", "processing", "reasoning",
        "analyzing", "computing", "synthesizing", "formulating", "brainstorming",
    };
    return kVerbs;
}

namespace {
const std::unordered_map<std::string, std::vector<std::string>>& spinner_table() {
    static const std::unordered_map<std::string, std::vector<std::string>> kTable = {
        {"dots",     {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"}},
        {"bounce",   {"⠁","⠂","⠄","⡀","⢀","⠠","⠐","⠈"}},
        {"grow",     {"▁","▂","▃","▄","▅","▆","▇","█","▇","▆","▅","▄","▃","▂"}},
        {"arrows",   {"←","↖","↑","↗","→","↘","↓","↙"}},
        {"star",     {"✶","✷","✸","✹","✺","✹","✸","✷"}},
        {"moon",     {"🌑","🌒","🌓","🌔","🌕","🌖","🌗","🌘"}},
        {"pulse",    {"◜","◠","◝","◞","◡","◟"}},
        {"brain",    {"🧠","💭","💡","✨","💫","🌟","💡","💭"}},
        {"sparkle",  {"⁺","˚","*","✧","✦","✧","*","˚"}},
    };
    return kTable;
}
}  // namespace

const std::vector<std::string>& spinner_frames(const std::string& kind) {
    auto it = spinner_table().find(kind);
    if (it != spinner_table().end()) return it->second;
    return spinner_table().at("dots");
}

std::vector<std::string> spinner_kinds() {
    std::vector<std::string> kinds;
    kinds.reserve(spinner_table().size());
    for (auto& kv : spinner_table()) kinds.push_back(kv.first);
    std::sort(kinds.begin(), kinds.end());
    return kinds;
}

}  // namespace hermes::agent::display
