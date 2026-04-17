// OpenAI-compatible shim that forwards Hermes chat requests to the
// GitHub Copilot CLI running as an ACP server (`copilot --acp --stdio`).
//
// This is a *reverse* adapter: Hermes acts as the ACP **client**, spawning
// a short-lived Copilot ACP subprocess per completion, formatting the
// conversation as a single prompt, collecting `session/update` text
// chunks, parsing any emitted `<tool_call>{...}</tool_call>` blocks, and
// returning a minimal OpenAI-shape response.  The Python reference lives
// at `agent/copilot_acp_client.py` (~570 LOC); the behaviour here mirrors
// that implementation one-to-one.
//
// Linux is the supported target.  POSIX fork/exec/pipe primitives are
// guarded so the translation unit still compiles cleanly under MSVC
// (the `complete()` entry point returns a spawn error on Windows).
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::agent {

// Base marker URL used by the Python implementation when Hermes needs to
// identify an ACP-backed OpenAI client.  Kept here so callers can compare
// against it without hard-coding the literal.
inline constexpr const char* kCopilotAcpMarkerBaseUrl = "acp://copilot";

// Default per-request timeout.  Matches the Python constant
// `_DEFAULT_TIMEOUT_SECONDS = 900.0`.
inline constexpr std::chrono::seconds kCopilotAcpDefaultTimeout{900};

struct CopilotACPToolCall {
    std::string id;
    std::string name;
    // OpenAI-style: an opaque JSON string, not a parsed object.
    std::string arguments;
};

struct CopilotACPResponse {
    std::string content;
    std::vector<CopilotACPToolCall> tool_calls;
    std::string reasoning;         // agent_thought_chunk content, if any
    std::string model;             // hint echoed back, defaults to "copilot-acp"
    std::string finish_reason;     // "stop" | "tool_calls" | "timeout" | "error"
    std::string error_message;     // populated when finish_reason == "error"
    std::optional<nlohmann::json> raw;
};

struct CopilotACPRequest {
    // OpenAI-shape array: [{"role":"user","content":"..."}, ...]
    nlohmann::json messages = nlohmann::json::array();
    // Optional OpenAI-shape tools array.
    std::optional<nlohmann::json> tools;
    std::optional<nlohmann::json> tool_choice;
    std::string model;
    std::chrono::seconds timeout = kCopilotAcpDefaultTimeout;
    // If empty, the current working directory is used.
    std::string cwd;
    // Extra KEY=VALUE pairs forwarded to the child environment.
    std::vector<std::string> extra_env;
};

class CopilotACPClient {
public:
    CopilotACPClient();
    ~CopilotACPClient();

    CopilotACPClient(const CopilotACPClient&) = delete;
    CopilotACPClient& operator=(const CopilotACPClient&) = delete;

    // One-shot completion: starts a short-lived ACP session, sends the
    // formatted prompt, collects text chunks, parses tool_calls, returns.
    // On any failure the response is populated with `finish_reason="error"`
    // and an informative `error_message` instead of throwing.
    CopilotACPResponse complete(const CopilotACPRequest& req);

    // Overrides.  When either is left at its default the corresponding
    // environment variable wins:
    //   HERMES_COPILOT_ACP_COMMAND / COPILOT_CLI_PATH  -> command
    //   HERMES_COPILOT_ACP_ARGS                        -> args (shell-split)
    // and finally falls back to `copilot` + ["--acp", "--stdio"].
    void set_command(std::string cmd);
    void set_args(std::vector<std::string> args);

    // Introspection helpers — useful in tests that want to assert the
    // resolved command line before spawning anything.
    std::string resolved_command() const;
    std::vector<std::string> resolved_args() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Top-level helpers — mirror the Python module-level functions so tests can
// verify them without spawning a subprocess.
// ---------------------------------------------------------------------------

// Environment-aware command resolution (same precedence as Python's
// `_resolve_command` / `_resolve_args`).
std::string resolve_copilot_acp_command();
std::vector<std::string> resolve_copilot_acp_args();

// Build the single prompt string sent to copilot.  Mirrors Python's
// `_format_messages_as_prompt(messages, model, tools, tool_choice)`.
std::string format_messages_as_prompt(
    const nlohmann::json& messages,
    const std::string& model = {},
    const std::optional<nlohmann::json>& tools = std::nullopt,
    const std::optional<nlohmann::json>& tool_choice = std::nullopt);

// Mirrors Python's `_render_message_content(content)`.  Accepts a string,
// dict (extracts "text"/"content" fallbacks), or list (OpenAI vision-style
// content parts).
std::string render_message_content(const nlohmann::json& content);

// Mirrors Python's `_extract_tool_calls_from_text(text)`.  Returns the
// parsed tool_calls and a cleaned text with the `<tool_call>` blocks
// stripped (and any matched bare-JSON fallbacks removed).
std::pair<std::vector<CopilotACPToolCall>, std::string>
extract_tool_calls_from_text(std::string_view text);

// Mirrors Python's `_ensure_path_within_cwd(path, cwd)`.  Throws
// std::runtime_error when `path` is outside the resolved cwd (or
// non-absolute).  Returns the canonicalised path.
std::string ensure_path_within_cwd(const std::string& path_text,
                                   const std::string& cwd);

// Build a JSON-RPC 2.0 error payload.  Mirrors Python's `_jsonrpc_error`.
nlohmann::json jsonrpc_error(const nlohmann::json& message_id,
                             int code,
                             const std::string& message);

}  // namespace hermes::agent
