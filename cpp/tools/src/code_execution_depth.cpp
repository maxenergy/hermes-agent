// Implementation of hermes/tools/code_execution_depth.hpp.
#include "hermes/tools/code_execution_depth.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>

namespace hermes::tools::code_execution::depth {

namespace {

std::string to_upper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string python_repr_dict(const nlohmann::json& obj) {
    // Approximate Python's ``str(dict)`` for short previews.  Keys are
    // emitted as quoted strings, values recursively.  Not a full
    // round-trip — we only need a stable truncated preview.
    std::ostringstream oss;
    if (obj.is_object()) {
        oss << "{";
        bool first = true;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!first) {
                oss << ", ";
            }
            first = false;
            oss << "'" << it.key() << "': ";
            oss << python_repr_dict(it.value());
        }
        oss << "}";
    } else if (obj.is_array()) {
        oss << "[";
        bool first = true;
        for (const auto& v : obj) {
            if (!first) {
                oss << ", ";
            }
            first = false;
            oss << python_repr_dict(v);
        }
        oss << "]";
    } else if (obj.is_string()) {
        oss << "'" << obj.get<std::string>() << "'";
    } else if (obj.is_boolean()) {
        oss << (obj.get<bool>() ? "True" : "False");
    } else if (obj.is_null()) {
        oss << "None";
    } else {
        oss << obj.dump();
    }
    return oss.str();
}

}  // namespace

// ---- Env filtering -----------------------------------------------------

const std::vector<std::string>& safe_env_prefixes() {
    static const std::vector<std::string> prefixes = {
        "PATH", "HOME", "USER", "LANG", "LC_",     "TERM",
        "TMPDIR", "TMP", "TEMP", "SHELL", "LOGNAME", "XDG_",
        "PYTHONPATH", "VIRTUAL_ENV", "CONDA",
    };
    return prefixes;
}

const std::vector<std::string>& secret_substrings() {
    static const std::vector<std::string> secrets = {
        "KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL", "PASSWD", "AUTH",
    };
    return secrets;
}

bool env_name_looks_like_secret(std::string_view name) {
    const std::string upper = to_upper(name);
    for (const auto& s : secret_substrings()) {
        if (upper.find(s) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool env_name_has_safe_prefix(std::string_view name) {
    for (const auto& p : safe_env_prefixes()) {
        if (name.size() >= p.size() && name.compare(0, p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

std::unordered_map<std::string, std::string> filter_child_env(
    const std::unordered_map<std::string, std::string>& src,
    EnvPassthroughFn passthrough) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& [k, v] : src) {
        if (passthrough != nullptr && passthrough(k)) {
            out[k] = v;
            continue;
        }
        if (env_name_looks_like_secret(k)) {
            continue;
        }
        if (env_name_has_safe_prefix(k)) {
            out[k] = v;
        }
    }
    return out;
}

// ---- Forbidden terminal params -----------------------------------------

const std::unordered_set<std::string>& forbidden_terminal_params() {
    static const std::unordered_set<std::string> s = {
        "background", "pty", "force", "notify_on_complete", "watch_patterns",
    };
    return s;
}

std::size_t strip_forbidden_terminal_params(nlohmann::json& args) {
    if (!args.is_object()) {
        return 0;
    }
    std::size_t dropped = 0;
    for (const auto& key : forbidden_terminal_params()) {
        if (args.erase(key) > 0) {
            ++dropped;
        }
    }
    return dropped;
}

// ---- RPC framing --------------------------------------------------------

ParsedRpcFrames parse_rpc_frames(std::string_view buffer) {
    ParsedRpcFrames out;
    std::size_t start = 0;
    while (start < buffer.size()) {
        const auto nl = buffer.find('\n', start);
        if (nl == std::string_view::npos) {
            out.residual.assign(buffer.substr(start));
            return out;
        }
        std::string_view line = buffer.substr(start, nl - start);
        start = nl + 1;
        // Trim the line.
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
            line.remove_prefix(1);
        }
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        try {
            out.frames.push_back(nlohmann::json::parse(line));
        } catch (const nlohmann::json::exception&) {
            ++out.parse_errors;
        }
    }
    return out;
}

std::string encode_rpc_reply(std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 1);
    out.append(payload);
    out.push_back('\n');
    return out;
}

std::string build_not_allowed_reply(
    std::string_view tool_name,
    const std::unordered_set<std::string>& allowed) {
    std::vector<std::string> sorted_allowed(allowed.begin(), allowed.end());
    std::sort(sorted_allowed.begin(), sorted_allowed.end());
    std::string joined;
    for (std::size_t i = 0; i < sorted_allowed.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += sorted_allowed[i];
    }
    std::string msg = "Tool '";
    msg += std::string{tool_name};
    msg += "' is not available in execute_code. Available: ";
    msg += joined;
    nlohmann::json reply;
    reply["error"] = std::move(msg);
    return reply.dump();
}

std::string build_limit_reached_reply(int max_calls) {
    nlohmann::json reply;
    std::string msg = "Tool call limit reached (";
    msg += std::to_string(max_calls);
    msg += "). No more tool calls allowed in this execution.";
    reply["error"] = std::move(msg);
    return reply.dump();
}

std::string build_invalid_rpc_reply(std::string_view parse_error) {
    nlohmann::json reply;
    std::string msg = "Invalid RPC request: ";
    msg += std::string{parse_error};
    reply["error"] = std::move(msg);
    return reply.dump();
}

// ---- Socket path -------------------------------------------------------

std::string resolve_socket_tmpdir(std::string_view platform,
                                  std::string_view tmpdir_fallback) {
    if (platform == "darwin") {
        return "/tmp";
    }
    return std::string{tmpdir_fallback};
}

std::string build_socket_path(std::string_view tmpdir, std::string_view hex_id) {
    std::string out{tmpdir};
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out += "hermes_rpc_";
    out += std::string{hex_id};
    out += ".sock";
    return out;
}

// ---- Tool call logging -------------------------------------------------

std::string format_args_preview(const nlohmann::json& args, std::size_t limit) {
    std::string repr;
    if (args.is_object() || args.is_array()) {
        repr = python_repr_dict(args);
    } else {
        repr = args.dump();
    }
    if (repr.size() <= limit) {
        return repr;
    }
    return repr.substr(0, limit);
}

double round_duration_seconds(double seconds) {
    return std::round(seconds * 100.0) / 100.0;
}

// ---- Schema description ------------------------------------------------

const std::vector<std::pair<std::string, std::string>>&
canonical_tool_doc_lines() {
    static const std::vector<std::pair<std::string, std::string>> lines = {
        {"web_search",
         "  web_search(query: str, limit: int = 5) -> dict\n"
         "    Returns {\"data\": {\"web\": [{\"url\", \"title\", "
         "\"description\"}, ...]}}"},
        {"web_extract",
         "  web_extract(urls: list[str]) -> dict\n"
         "    Returns {\"results\": [{\"url\", \"title\", \"content\", "
         "\"error\"}, ...]} where content is markdown"},
        {"read_file",
         "  read_file(path: str, offset: int = 1, limit: int = 500) -> dict\n"
         "    Lines are 1-indexed. Returns {\"content\": \"...\", "
         "\"total_lines\": N}"},
        {"write_file",
         "  write_file(path: str, content: str) -> dict\n"
         "    Always overwrites the entire file."},
        {"search_files",
         "  search_files(pattern: str, target=\"content\", path=\".\", "
         "file_glob=None, limit=50) -> dict\n"
         "    target: \"content\" (search inside files) or \"files\" (find "
         "files by name). Returns {\"matches\": [...]}"},
        {"patch",
         "  patch(path: str, old_string: str, new_string: str, replace_all: "
         "bool = False) -> dict\n"
         "    Replaces old_string with new_string in the file."},
        {"terminal",
         "  terminal(command: str, timeout=None, workdir=None) -> dict\n"
         "    Foreground only (no background/pty). Returns {\"output\": "
         "\"...\", \"exit_code\": N}"},
    };
    return lines;
}

std::vector<std::pair<std::string, std::string>>
filter_tool_doc_lines(const std::unordered_set<std::string>& enabled) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, doc] : canonical_tool_doc_lines()) {
        if (enabled.count(name) > 0) {
            out.emplace_back(name, doc);
        }
    }
    return out;
}

std::string build_import_examples(
    const std::unordered_set<std::string>& enabled) {
    // Prefer web_search, terminal if available.
    std::vector<std::string> preferred;
    for (const auto* name : {"web_search", "terminal"}) {
        if (enabled.count(name) > 0) {
            preferred.emplace_back(name);
        }
    }
    if (preferred.empty()) {
        std::vector<std::string> sorted(enabled.begin(), enabled.end());
        std::sort(sorted.begin(), sorted.end());
        if (sorted.empty()) {
            return "...";
        }
        const std::size_t take = std::min<std::size_t>(2, sorted.size());
        for (std::size_t i = 0; i < take; ++i) {
            preferred.emplace_back(sorted[i]);
        }
    }
    std::string joined;
    for (std::size_t i = 0; i < preferred.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += preferred[i];
    }
    joined += ", ...";
    return joined;
}

// ---- Misc --------------------------------------------------------------

std::string windows_unsupported_reply() {
    nlohmann::json reply;
    reply["error"] =
        "execute_code is not available on Windows. Use normal tool calls "
        "instead.";
    return reply.dump();
}

int clamp_max_tool_calls(int requested, int default_value) {
    int v = requested;
    if (v <= 0) {
        v = default_value;
    }
    if (v < kMinToolCalls) {
        v = kMinToolCalls;
    }
    if (v > kMaxToolCalls) {
        v = kMaxToolCalls;
    }
    return v;
}

}  // namespace hermes::tools::code_execution::depth
