// Implementation of the Copilot ACP reverse adapter.  Mirrors
// `agent/copilot_acp_client.py` from the Python code base.
//
// Bidirectional JSON-RPC over the child's stdio is required (the server
// can send `session/request_permission`, `fs/read_text_file`, etc. during
// a `session/prompt`), so we can't use `run_capture` — it closes the
// child's stdin before the response stream begins.  Instead we use a
// hand-rolled POSIX fork/execvp/pipe transport, patterned after
// `cpp/tools/src/mcp_transport.cpp`.  On non-POSIX platforms the
// `complete()` entry point degrades gracefully to an error response so
// the translation unit still compiles.

#include "hermes/agent/copilot_acp_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace hermes::agent {

namespace {

using nlohmann::json;

// ─── shlex.split approximation ──────────────────────────────────────────
//
// Handles single quotes, double quotes, and backslash escapes.  Good
// enough for the `HERMES_COPILOT_ACP_ARGS` env var (the Python side uses
// `shlex.split` directly).
std::vector<std::string> shell_split(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    enum class Q { None, Single, Double };
    Q q = Q::None;

    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (q == Q::Single) {
            if (c == '\'') { q = Q::None; }
            else { cur.push_back(c); in_token = true; }
            continue;
        }
        if (q == Q::Double) {
            if (c == '\\' && i + 1 < text.size()) {
                char n = text[i + 1];
                if (n == '"' || n == '\\' || n == '$' || n == '`' || n == '\n') {
                    cur.push_back(n);
                    ++i;
                    continue;
                }
                cur.push_back(c);
                continue;
            }
            if (c == '"') { q = Q::None; }
            else { cur.push_back(c); in_token = true; }
            continue;
        }
        // Unquoted.
        if (c == '\\' && i + 1 < text.size()) {
            cur.push_back(text[i + 1]);
            in_token = true;
            ++i;
            continue;
        }
        if (c == '\'') { q = Q::Single; in_token = true; continue; }
        if (c == '"')  { q = Q::Double; in_token = true; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (in_token) {
                out.push_back(std::move(cur));
                cur.clear();
                in_token = false;
            }
            continue;
        }
        cur.push_back(c);
        in_token = true;
    }
    if (in_token) out.push_back(std::move(cur));
    return out;
}

std::string strip(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? strip(std::string(v)) : std::string{};
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ─── Tool-call regex patterns ───────────────────────────────────────────
//
// Compiled once, reused.  Patterns mirror Python's `_TOOL_CALL_BLOCK_RE`
// and `_TOOL_CALL_JSON_RE` with an equivalent non-greedy DOTALL match.
//
// std::regex does not natively support `(?s)` (dot-matches-newline); we
// compile with ECMAScript and emulate `.` with `[\s\S]`.
const std::regex& tool_block_regex() {
    static const std::regex re(
        R"(<tool_call>\s*(\{[\s\S]*?\})\s*</tool_call>)",
        std::regex::ECMAScript);
    return re;
}

const std::regex& tool_json_regex() {
    static const std::regex re(
        R"(\{\s*"id"\s*:\s*"[^"]+"\s*,\s*"type"\s*:\s*"function"\s*,\s*"function"\s*:\s*\{[\s\S]*?\}\s*\})",
        std::regex::ECMAScript);
    return re;
}

void try_add_tool_call(const std::string& raw_json,
                       std::vector<CopilotACPToolCall>& out) {
    json obj;
    try {
        obj = json::parse(raw_json);
    } catch (const std::exception&) {
        return;
    }
    if (!obj.is_object()) return;
    auto fn_it = obj.find("function");
    if (fn_it == obj.end() || !fn_it->is_object()) return;
    auto name_it = fn_it->find("name");
    if (name_it == fn_it->end() || !name_it->is_string()) return;
    std::string fn_name = strip(name_it->get<std::string>());
    if (fn_name.empty()) return;

    std::string fn_args;
    auto args_it = fn_it->find("arguments");
    if (args_it == fn_it->end()) {
        fn_args = "{}";
    } else if (args_it->is_string()) {
        fn_args = args_it->get<std::string>();
    } else {
        fn_args = args_it->dump();
    }

    std::string call_id;
    auto id_it = obj.find("id");
    if (id_it != obj.end() && id_it->is_string()) {
        call_id = strip(id_it->get<std::string>());
    }
    if (call_id.empty()) {
        call_id = "acp_call_" + std::to_string(out.size() + 1);
    }

    out.push_back(CopilotACPToolCall{std::move(call_id), std::move(fn_name), std::move(fn_args)});
}

}  // namespace

// ─── Top-level helpers ──────────────────────────────────────────────────

std::string resolve_copilot_acp_command() {
    auto v = env_or_empty("HERMES_COPILOT_ACP_COMMAND");
    if (!v.empty()) return v;
    v = env_or_empty("COPILOT_CLI_PATH");
    if (!v.empty()) return v;
    return "copilot";
}

std::vector<std::string> resolve_copilot_acp_args() {
    auto raw = env_or_empty("HERMES_COPILOT_ACP_ARGS");
    if (raw.empty()) return {"--acp", "--stdio"};
    auto split = shell_split(raw);
    if (split.empty()) return {"--acp", "--stdio"};
    return split;
}

std::string render_message_content(const json& content) {
    if (content.is_null()) return {};
    if (content.is_string()) return strip(content.get<std::string>());
    if (content.is_object()) {
        auto text_it = content.find("text");
        if (text_it != content.end() && text_it->is_string()) {
            return strip(text_it->get<std::string>());
        }
        auto inner_it = content.find("content");
        if (inner_it != content.end() && inner_it->is_string()) {
            return strip(inner_it->get<std::string>());
        }
        return content.dump();
    }
    if (content.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : content) {
            if (item.is_string()) {
                parts.push_back(item.get<std::string>());
            } else if (item.is_object()) {
                auto t = item.find("text");
                if (t != item.end() && t->is_string()) {
                    auto s = strip(t->get<std::string>());
                    if (!s.empty()) parts.push_back(std::move(s));
                }
            }
        }
        std::string joined;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) joined.push_back('\n');
            joined += parts[i];
        }
        return strip(joined);
    }
    return strip(content.dump());
}

std::string format_messages_as_prompt(
    const json& messages,
    const std::string& model,
    const std::optional<json>& tools,
    const std::optional<json>& tool_choice) {
    std::vector<std::string> sections;
    sections.emplace_back("You are being used as the active ACP agent backend for Hermes.");
    sections.emplace_back("Use ACP capabilities to complete tasks.");
    sections.emplace_back(
        "IMPORTANT: If you take an action with a tool, you MUST output tool calls using "
        "<tool_call>{...}</tool_call> blocks with JSON exactly in OpenAI function-call shape.");
    sections.emplace_back("If no tool is needed, answer normally.");

    if (!model.empty()) {
        sections.emplace_back("Hermes requested model hint: " + model);
    }

    if (tools && tools->is_array() && !tools->empty()) {
        json tool_specs = json::array();
        for (const auto& t : *tools) {
            if (!t.is_object()) continue;
            auto fn_it = t.find("function");
            if (fn_it == t.end() || !fn_it->is_object()) continue;
            auto name_it = fn_it->find("name");
            if (name_it == fn_it->end() || !name_it->is_string()) continue;
            std::string name = strip(name_it->get<std::string>());
            if (name.empty()) continue;
            json spec;
            spec["name"] = name;
            auto desc_it = fn_it->find("description");
            spec["description"] = (desc_it != fn_it->end()) ? *desc_it : json("");
            auto params_it = fn_it->find("parameters");
            spec["parameters"] = (params_it != fn_it->end()) ? *params_it : json::object();
            tool_specs.push_back(std::move(spec));
        }
        if (!tool_specs.empty()) {
            std::string section =
                "Available tools (OpenAI function schema). "
                "When using a tool, emit ONLY <tool_call>{...}</tool_call> with one JSON object "
                "containing id/type/function{name,arguments}. arguments must be a JSON string.\n" +
                tool_specs.dump();
            sections.push_back(std::move(section));
        }
    }

    if (tool_choice && !tool_choice->is_null()) {
        sections.emplace_back("Tool choice hint: " + tool_choice->dump());
    }

    std::vector<std::string> transcript;
    if (messages.is_array()) {
        for (const auto& message : messages) {
            if (!message.is_object()) continue;
            std::string role = "unknown";
            auto role_it = message.find("role");
            if (role_it != message.end() && role_it->is_string()) {
                role = strip(role_it->get<std::string>());
            }
            role = to_lower(role);
            if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
                role = "context";
            }

            json content = json(nullptr);
            auto c_it = message.find("content");
            if (c_it != message.end()) content = *c_it;
            std::string rendered = render_message_content(content);
            if (rendered.empty()) continue;

            std::string label;
            if (role == "system") label = "System";
            else if (role == "user") label = "User";
            else if (role == "assistant") label = "Assistant";
            else if (role == "tool") label = "Tool";
            else if (role == "context") label = "Context";
            else {
                label = role;
                if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            }
            transcript.push_back(label + ":\n" + rendered);
        }
    }

    if (!transcript.empty()) {
        std::string joined = "Conversation transcript:\n\n";
        for (std::size_t i = 0; i < transcript.size(); ++i) {
            if (i) joined += "\n\n";
            joined += transcript[i];
        }
        sections.push_back(std::move(joined));
    }

    sections.emplace_back("Continue the conversation from the latest user request.");

    std::string out;
    bool first = true;
    for (auto& sec : sections) {
        auto s = strip(sec);
        if (s.empty()) continue;
        if (!first) out += "\n\n";
        out += s;
        first = false;
    }
    return out;
}

std::pair<std::vector<CopilotACPToolCall>, std::string>
extract_tool_calls_from_text(std::string_view text) {
    std::vector<CopilotACPToolCall> extracted;
    std::string s(text);
    if (strip(s).empty()) return {extracted, {}};

    std::vector<std::pair<std::size_t, std::size_t>> consumed;

    // <tool_call>{...}</tool_call> blocks.
    {
        auto begin = std::sregex_iterator(s.begin(), s.end(), tool_block_regex());
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            try_add_tool_call(m.str(1), extracted);
            consumed.emplace_back(
                static_cast<std::size_t>(m.position(0)),
                static_cast<std::size_t>(m.position(0) + m.length(0)));
        }
    }

    // Fallback to bare-JSON matches only when no XML blocks were found.
    if (extracted.empty()) {
        auto begin = std::sregex_iterator(s.begin(), s.end(), tool_json_regex());
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            try_add_tool_call(m.str(0), extracted);
            consumed.emplace_back(
                static_cast<std::size_t>(m.position(0)),
                static_cast<std::size_t>(m.position(0) + m.length(0)));
        }
    }

    if (consumed.empty()) return {extracted, strip(s)};

    std::sort(consumed.begin(), consumed.end());
    std::vector<std::pair<std::size_t, std::size_t>> merged;
    for (const auto& span : consumed) {
        if (merged.empty() || span.first > merged.back().second) {
            merged.push_back(span);
        } else {
            merged.back().second = std::max(merged.back().second, span.second);
        }
    }

    std::vector<std::string> parts;
    std::size_t cursor = 0;
    for (const auto& span : merged) {
        if (cursor < span.first) {
            parts.push_back(s.substr(cursor, span.first - cursor));
        }
        cursor = std::max(cursor, span.second);
    }
    if (cursor < s.size()) parts.push_back(s.substr(cursor));

    std::string cleaned;
    bool first = true;
    for (auto& p : parts) {
        auto trimmed = strip(p);
        if (trimmed.empty()) continue;
        if (!first) cleaned.push_back('\n');
        cleaned += trimmed;
        first = false;
    }
    return {extracted, strip(cleaned)};
}

std::string ensure_path_within_cwd(const std::string& path_text,
                                   const std::string& cwd) {
    namespace fs = std::filesystem;
    fs::path candidate(path_text);
    if (!candidate.is_absolute()) {
        throw std::runtime_error("ACP file-system paths must be absolute.");
    }
    std::error_code ec;
    auto resolved = fs::weakly_canonical(candidate, ec);
    if (ec) resolved = candidate.lexically_normal();
    auto root = fs::weakly_canonical(fs::path(cwd), ec);
    if (ec) root = fs::path(cwd).lexically_normal();

    auto rel = resolved.lexically_relative(root);
    auto rel_str = rel.string();
    if (rel_str.empty() || rel_str.rfind("..", 0) == 0) {
        throw std::runtime_error(
            "Path '" + resolved.string() + "' is outside the session cwd '" +
            root.string() + "'.");
    }
    return resolved.string();
}

json jsonrpc_error(const json& message_id, int code, const std::string& message) {
    json payload;
    payload["jsonrpc"] = "2.0";
    payload["id"] = message_id;
    payload["error"] = json::object();
    payload["error"]["code"] = code;
    payload["error"]["message"] = message;
    return payload;
}

// ─── Impl: subprocess + JSON-RPC client ────────────────────────────────

struct CopilotACPClient::Impl {
    std::string command_override;
    std::vector<std::string> args_override;
    bool args_overridden = false;

    std::string effective_command() const {
        if (!command_override.empty()) return command_override;
        return resolve_copilot_acp_command();
    }
    std::vector<std::string> effective_args() const {
        if (args_overridden) return args_override;
        return resolve_copilot_acp_args();
    }

#if !defined(_WIN32)
    // ── One-shot ACP conversation ────────────────────────────────────────
    CopilotACPResponse run(const CopilotACPRequest& req, const std::string& prompt_text);

    static void install_sigpipe_ignore() {
        static std::once_flag flag;
        std::call_once(flag, []() { ::signal(SIGPIPE, SIG_IGN); });
    }
#endif
};

CopilotACPClient::CopilotACPClient() : impl_(std::make_unique<Impl>()) {}
CopilotACPClient::~CopilotACPClient() = default;

void CopilotACPClient::set_command(std::string cmd) {
    impl_->command_override = std::move(cmd);
}
void CopilotACPClient::set_args(std::vector<std::string> args) {
    impl_->args_override = std::move(args);
    impl_->args_overridden = true;
}
std::string CopilotACPClient::resolved_command() const {
    return impl_->effective_command();
}
std::vector<std::string> CopilotACPClient::resolved_args() const {
    return impl_->effective_args();
}

#if defined(_WIN32)

CopilotACPResponse CopilotACPClient::complete(const CopilotACPRequest& req) {
    CopilotACPResponse r;
    r.model = req.model.empty() ? std::string("copilot-acp") : req.model;
    r.finish_reason = "error";
    r.error_message =
        "Copilot ACP client requires POSIX; subprocess support is not implemented on this platform.";
    return r;
}

#else

namespace {

// RAII helper for parent-side fds.
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { reset(); fd = other.fd; other.fd = -1; }
        return *this;
    }
    void reset(int nf = -1) {
        if (fd >= 0) ::close(fd);
        fd = nf;
    }
    int release() { int f = fd; fd = -1; return f; }
};

// Read ready-to-parse JSON-RPC messages from a pipe, framed by newlines.
// Returns the parsed message, or std::nullopt on timeout.  Sets *eof=true
// when the child closed its stdout.
struct RpcReader {
    int fd;
    std::string buffer;

    std::optional<json> read_once(std::chrono::milliseconds remaining, bool& eof) {
        eof = false;
        // Flush any buffered complete line first.
        auto nl = buffer.find('\n');
        while (nl != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            auto trimmed = strip(line);
            if (trimmed.empty()) { nl = buffer.find('\n'); continue; }
            try {
                return json::parse(trimmed);
            } catch (const std::exception&) {
                // Non-JSON line — return as {"raw": line}.
                json raw;
                raw["raw"] = trimmed;
                return raw;
            }
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;

        int ms = static_cast<int>(std::max<std::int64_t>(0, remaining.count()));
        int ret = ::poll(&pfd, 1, ms);
        if (ret < 0) {
            if (errno == EINTR) return std::nullopt;
            eof = true;
            return std::nullopt;
        }
        if (ret == 0) return std::nullopt;  // timeout
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) return std::nullopt;
                eof = true;
                return std::nullopt;
            }
            if (n == 0) { eof = true; return std::nullopt; }
            buffer.append(buf, static_cast<std::size_t>(n));
        }
        auto nl2 = buffer.find('\n');
        if (nl2 != std::string::npos) {
            std::string line = buffer.substr(0, nl2);
            buffer.erase(0, nl2 + 1);
            auto trimmed = strip(line);
            if (trimmed.empty()) return std::nullopt;
            try {
                return json::parse(trimmed);
            } catch (const std::exception&) {
                json raw;
                raw["raw"] = trimmed;
                return raw;
            }
        }
        return std::nullopt;
    }
};

bool write_all(int fd, const std::string& data) {
    std::size_t remaining = data.size();
    const char* p = data.data();
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

// Handle a server-initiated message.  Returns true if the message was a
// server-side notification/request that we dispatched (or swallowed), so
// the caller should keep polling; false if the caller should treat this
// as a response to its pending request.
bool handle_server_message(
    const json& msg,
    int stdin_fd,
    const std::string& cwd,
    std::string& text_parts,
    std::string& reasoning_parts) {
    auto method_it = msg.find("method");
    if (method_it == msg.end() || !method_it->is_string()) return false;
    const std::string method = method_it->get<std::string>();

    if (method == "session/update") {
        auto params_it = msg.find("params");
        if (params_it == msg.end() || !params_it->is_object()) return true;
        auto update_it = params_it->find("update");
        if (update_it == params_it->end() || !update_it->is_object()) return true;
        std::string kind;
        auto kind_it = update_it->find("sessionUpdate");
        if (kind_it != update_it->end() && kind_it->is_string()) {
            kind = strip(kind_it->get<std::string>());
        }
        std::string chunk_text;
        auto content_it = update_it->find("content");
        if (content_it != update_it->end() && content_it->is_object()) {
            auto text_it = content_it->find("text");
            if (text_it != content_it->end() && text_it->is_string()) {
                chunk_text = text_it->get<std::string>();
            }
        }
        if (kind == "agent_message_chunk" && !chunk_text.empty()) {
            text_parts += chunk_text;
        } else if (kind == "agent_thought_chunk" && !chunk_text.empty()) {
            reasoning_parts += chunk_text;
        }
        return true;
    }

    json message_id = json(nullptr);
    auto id_it = msg.find("id");
    if (id_it != msg.end()) message_id = *id_it;

    json params = json::object();
    auto params_it = msg.find("params");
    if (params_it != msg.end() && params_it->is_object()) params = *params_it;

    json response;

    if (method == "session/request_permission") {
        response = json::object();
        response["jsonrpc"] = "2.0";
        response["id"] = message_id;
        response["result"] = json::object();
        response["result"]["outcome"] = json::object();
        response["result"]["outcome"]["outcome"] = "allow_once";
    } else if (method == "fs/read_text_file") {
        try {
            std::string path_text;
            auto p_it = params.find("path");
            if (p_it != params.end() && p_it->is_string()) {
                path_text = p_it->get<std::string>();
            }
            std::string resolved = ensure_path_within_cwd(path_text, cwd);
            std::string content;
            std::error_code ec;
            if (std::filesystem::exists(resolved, ec)) {
                std::ifstream ifs(resolved, std::ios::in | std::ios::binary);
                if (ifs) {
                    std::ostringstream oss;
                    oss << ifs.rdbuf();
                    content = oss.str();
                }
            }
            auto line_it = params.find("line");
            auto limit_it = params.find("limit");
            if (line_it != params.end() && line_it->is_number_integer()) {
                int line = line_it->get<int>();
                if (line > 1) {
                    std::vector<std::string> lines;
                    std::string cur;
                    for (char c : content) {
                        cur.push_back(c);
                        if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
                    }
                    if (!cur.empty()) lines.push_back(std::move(cur));
                    std::size_t start = static_cast<std::size_t>(line - 1);
                    std::size_t end = lines.size();
                    if (limit_it != params.end() && limit_it->is_number_integer()) {
                        int limit = limit_it->get<int>();
                        if (limit > 0) {
                            end = std::min(lines.size(), start + static_cast<std::size_t>(limit));
                        }
                    }
                    std::string sliced;
                    for (std::size_t i = start; i < end && i < lines.size(); ++i) {
                        sliced += lines[i];
                    }
                    content = std::move(sliced);
                }
            }
            response = json::object();
            response["jsonrpc"] = "2.0";
            response["id"] = message_id;
            response["result"] = json::object();
            response["result"]["content"] = content;
        } catch (const std::exception& exc) {
            response = jsonrpc_error(message_id, -32602, exc.what());
        }
    } else if (method == "fs/write_text_file") {
        try {
            std::string path_text;
            auto p_it = params.find("path");
            if (p_it != params.end() && p_it->is_string()) {
                path_text = p_it->get<std::string>();
            }
            std::string resolved = ensure_path_within_cwd(path_text, cwd);
            std::filesystem::path fp(resolved);
            std::error_code ec;
            std::filesystem::create_directories(fp.parent_path(), ec);
            std::string content_str;
            auto c_it = params.find("content");
            if (c_it != params.end() && c_it->is_string()) {
                content_str = c_it->get<std::string>();
            }
            std::ofstream ofs(resolved, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("Unable to open file for writing: " + resolved);
            ofs.write(content_str.data(), static_cast<std::streamsize>(content_str.size()));
            response = json::object();
            response["jsonrpc"] = "2.0";
            response["id"] = message_id;
            response["result"] = json(nullptr);
        } catch (const std::exception& exc) {
            response = jsonrpc_error(message_id, -32602, exc.what());
        }
    } else {
        response = jsonrpc_error(
            message_id,
            -32601,
            std::string("ACP client method '") + method + "' is not supported by Hermes yet.");
    }

    // Best-effort: ignore write failures; the outer poll loop will surface
    // the child exit if it's dead.
    (void)write_all(stdin_fd, response.dump() + "\n");
    return true;
}

}  // namespace

CopilotACPResponse CopilotACPClient::Impl::run(const CopilotACPRequest& req,
                                               const std::string& prompt_text) {
    CopilotACPResponse r;
    r.model = req.model.empty() ? std::string("copilot-acp") : req.model;

    install_sigpipe_ignore();

    const std::string command = effective_command();
    const std::vector<std::string> args = effective_args();

    std::string cwd = req.cwd;
    if (cwd.empty()) {
        std::error_code ec;
        cwd = std::filesystem::current_path(ec).string();
    }

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(to_child) != 0 || ::pipe(from_child) != 0 || ::pipe(err_pipe) != 0) {
        r.finish_reason = "error";
        r.error_message = std::string("pipe() failed: ") + std::strerror(errno);
        for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1], err_pipe[0], err_pipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
        return r;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        r.finish_reason = "error";
        r.error_message = std::string("fork() failed: ") + std::strerror(errno);
        for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1], err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return r;
    }

    if (pid == 0) {
        // -- Child --
        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(to_child[0]); ::close(to_child[1]);
        ::close(from_child[0]); ::close(from_child[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);

        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) {
                std::string err = std::string("chdir(") + cwd + "): " + std::strerror(errno) + "\n";
                (void)::write(STDERR_FILENO, err.data(), err.size());
                std::_Exit(127);
            }
        }
        for (const auto& kv : req.extra_env) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }

        std::vector<char*> argv;
        std::string cmd_copy = command;
        argv.push_back(cmd_copy.data());
        std::vector<std::string> args_copy(args.begin(), args.end());
        for (auto& a : args_copy) argv.push_back(a.data());
        argv.push_back(nullptr);

        ::execvp(command.c_str(), argv.data());
        std::string err = std::string("execvp(") + command + "): " + std::strerror(errno) + "\n";
        (void)::write(STDERR_FILENO, err.data(), err.size());
        std::_Exit(127);
    }

    // -- Parent --
    Fd stdin_fd(to_child[1]);      ::close(to_child[0]);
    Fd stdout_fd(from_child[0]);   ::close(from_child[1]);
    Fd stderr_fd(err_pipe[0]);     ::close(err_pipe[1]);

    // Non-blocking on stderr so we can drain it alongside stdout.
    {
        int f = ::fcntl(stderr_fd.fd, F_GETFL, 0);
        if (f >= 0) ::fcntl(stderr_fd.fd, F_SETFL, f | O_NONBLOCK);
    }
    {
        int f = ::fcntl(stdout_fd.fd, F_GETFL, 0);
        if (f >= 0) ::fcntl(stdout_fd.fd, F_SETFL, f | O_NONBLOCK);
    }

    RpcReader reader{stdout_fd.fd, {}};
    std::string stderr_tail;

    auto drain_stderr = [&]() {
        char buf[4096];
        while (true) {
            ssize_t n = ::read(stderr_fd.fd, buf, sizeof(buf));
            if (n <= 0) break;
            stderr_tail.append(buf, static_cast<std::size_t>(n));
            if (stderr_tail.size() > 8192) {
                stderr_tail.erase(0, stderr_tail.size() - 8192);
            }
        }
    };

    auto reap = [&](bool wait_blocking) -> std::optional<int> {
        int status = 0;
        pid_t w = ::waitpid(pid, &status, wait_blocking ? 0 : WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 0;
        }
        return std::nullopt;
    };

    bool child_exited = false;
    int exit_code = -1;
    std::string text_parts;
    std::string reasoning_parts;
    std::string timeout_message;
    std::string fatal_error;

    const auto deadline = std::chrono::steady_clock::now() + req.timeout;
    int next_id = 0;

    auto now_remaining = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    };

    // Send a request and await its response, dispatching server-initiated
    // messages along the way.  Returns the JSON result or std::nullopt on
    // timeout / fatal error (in which case fatal_error is populated).
    auto send_request = [&](const std::string& method, const json& params) -> std::optional<json> {
        int id = ++next_id;
        json payload;
        payload["jsonrpc"] = "2.0";
        payload["id"] = id;
        payload["method"] = method;
        payload["params"] = params;
        if (!write_all(stdin_fd.fd, payload.dump() + "\n")) {
            fatal_error = "Failed to write JSON-RPC frame to copilot stdin.";
            return std::nullopt;
        }

        while (true) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timeout_message = "Timed out waiting for Copilot ACP response to " + method + ".";
                return std::nullopt;
            }
            auto exited = reap(false);
            if (exited.has_value()) {
                child_exited = true;
                exit_code = *exited;
                drain_stderr();
                fatal_error = "Copilot ACP process exited before responding to " + method;
                if (!stderr_tail.empty()) fatal_error += ": " + strip(stderr_tail);
                return std::nullopt;
            }
            drain_stderr();

            auto slice = std::min<std::chrono::milliseconds>(now_remaining(), std::chrono::milliseconds(100));
            if (slice.count() <= 0) slice = std::chrono::milliseconds(50);
            bool eof = false;
            auto msg = reader.read_once(slice, eof);
            if (eof) {
                child_exited = true;
                auto ex = reap(true);
                if (ex.has_value()) exit_code = *ex;
                drain_stderr();
                fatal_error = "Copilot ACP process closed stdout before responding to " + method;
                if (!stderr_tail.empty()) fatal_error += ": " + strip(stderr_tail);
                return std::nullopt;
            }
            if (!msg.has_value()) continue;

            // Dispatch server-initiated messages.
            if (handle_server_message(*msg, stdin_fd.fd, cwd, text_parts, reasoning_parts)) {
                continue;
            }
            auto resp_id_it = msg->find("id");
            if (resp_id_it == msg->end()) continue;
            if (!resp_id_it->is_number_integer() || resp_id_it->get<int>() != id) continue;
            auto err_it = msg->find("error");
            if (err_it != msg->end()) {
                std::string em = "Copilot ACP " + method + " failed";
                if (err_it->is_object()) {
                    auto mm = err_it->find("message");
                    if (mm != err_it->end() && mm->is_string()) em += ": " + mm->get<std::string>();
                }
                fatal_error = em;
                return std::nullopt;
            }
            auto res_it = msg->find("result");
            if (res_it == msg->end()) return json(nullptr);
            return *res_it;
        }
    };

    // Protocol: initialize → session/new → session/prompt.
    do {
        json init_params;
        init_params["protocolVersion"] = 1;
        init_params["clientCapabilities"] = json::object();
        init_params["clientCapabilities"]["fs"] = json::object();
        init_params["clientCapabilities"]["fs"]["readTextFile"] = true;
        init_params["clientCapabilities"]["fs"]["writeTextFile"] = true;
        init_params["clientInfo"] = json::object();
        init_params["clientInfo"]["name"] = "hermes-agent";
        init_params["clientInfo"]["title"] = "Hermes Agent";
        init_params["clientInfo"]["version"] = "0.0.0";
        auto init_res = send_request("initialize", init_params);
        if (!init_res.has_value()) break;

        json session_params;
        session_params["cwd"] = cwd;
        session_params["mcpServers"] = json::array();
        auto session_res = send_request("session/new", session_params);
        if (!session_res.has_value()) break;
        std::string session_id;
        if (session_res->is_object()) {
            auto sid = session_res->find("sessionId");
            if (sid != session_res->end() && sid->is_string()) {
                session_id = strip(sid->get<std::string>());
            }
        }
        if (session_id.empty()) {
            fatal_error = "Copilot ACP did not return a sessionId.";
            break;
        }

        json prompt_params;
        prompt_params["sessionId"] = session_id;
        prompt_params["prompt"] = json::array();
        json chunk;
        chunk["type"] = "text";
        chunk["text"] = prompt_text;
        prompt_params["prompt"].push_back(chunk);
        (void)send_request("session/prompt", prompt_params);
    } while (false);

    // Close stdin to signal end-of-input and let the child drain.
    stdin_fd.reset();

    // Let the child wind down (bounded).
    if (!child_exited) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < end) {
            auto ex = reap(false);
            if (ex.has_value()) { child_exited = true; exit_code = *ex; break; }
            drain_stderr();
            pollfd pfd{stdout_fd.fd, POLLIN, 0};
            ::poll(&pfd, 1, 25);
            if (pfd.revents & (POLLIN | POLLHUP)) {
                bool eof = false;
                auto msg = reader.read_once(std::chrono::milliseconds(0), eof);
                if (msg.has_value()) {
                    handle_server_message(*msg, -1, cwd, text_parts, reasoning_parts);
                }
                if (eof) break;
            }
        }
    }
    if (!child_exited) {
        ::kill(pid, SIGTERM);
        auto ex = reap(false);
        if (!ex.has_value()) {
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < end) {
                ex = reap(false);
                if (ex.has_value()) break;
                pollfd pfd{stdout_fd.fd, POLLIN, 0};
                ::poll(&pfd, 1, 20);
            }
        }
        if (!ex.has_value()) {
            ::kill(pid, SIGKILL);
            ex = reap(true);
        }
        if (ex.has_value()) { child_exited = true; exit_code = *ex; }
    }
    drain_stderr();

    // Parse tool_calls from collected text.
    auto parsed = extract_tool_calls_from_text(text_parts);
    r.tool_calls = std::move(parsed.first);
    r.content = std::move(parsed.second);
    r.reasoning = reasoning_parts;

    if (!timeout_message.empty()) {
        r.finish_reason = "timeout";
        r.error_message = timeout_message;
    } else if (!fatal_error.empty()) {
        r.finish_reason = "error";
        r.error_message = fatal_error;
    } else if (exit_code != 0 && r.content.empty() && r.tool_calls.empty()) {
        r.finish_reason = "error";
        r.error_message = "Copilot ACP exited with status " + std::to_string(exit_code);
        if (!stderr_tail.empty()) r.error_message += ": " + strip(stderr_tail);
    } else if (!r.tool_calls.empty()) {
        r.finish_reason = "tool_calls";
    } else {
        r.finish_reason = "stop";
    }

    json raw;
    raw["exit_code"] = exit_code;
    raw["stderr"] = strip(stderr_tail);
    raw["text"] = text_parts;
    if (!reasoning_parts.empty()) raw["reasoning"] = reasoning_parts;
    r.raw = std::move(raw);
    return r;
}

CopilotACPResponse CopilotACPClient::complete(const CopilotACPRequest& req) {
    std::string prompt = format_messages_as_prompt(
        req.messages, req.model, req.tools, req.tool_choice);
    return impl_->run(req, prompt);
}

#endif  // !_WIN32

}  // namespace hermes::agent
