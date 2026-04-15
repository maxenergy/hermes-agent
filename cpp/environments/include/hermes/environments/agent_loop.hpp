// Portable surface of environments/agent_loop.py for the C++ backend.
//
// The full Python loop drives an OpenAI ChatCompletion server through a
// thread-pool tool dispatcher.  In the C++ port we expose:
//   * AgentResult / ToolError data types
//   * Reasoning extractor (multi-format provider compat)
//   * Tool-call normaliser (object | dict ↦ canonical OpenAI dict)
//   * User-task extractor (first non-empty user message)
//   * Slow-tool / unknown-tool / invalid-JSON helper checks
//
// The actual conversation loop / event loop / thread pool lives in the
// runtime layer — these helpers let it be tested in isolation.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hermes::environments::agent_loop {

// One tool execution error captured during the agent loop.
struct ToolError {
    int turn = 0;
    std::string tool_name;
    std::string arguments;   // truncated to ≤200 chars
    std::string error;
    std::string tool_result;
};

// Result of a full agent loop execution.
struct AgentResult {
    nlohmann::json messages = nlohmann::json::array();
    std::optional<nlohmann::json> managed_state;
    int turns_used = 0;
    bool finished_naturally = false;
    // One entry per turn.  Null entries denote "no reasoning emitted".
    std::vector<std::optional<std::string>> reasoning_per_turn;
    std::vector<ToolError> tool_errors;
};

// Provider-agnostic reasoning extraction.  Looks for, in order:
//   1. message.reasoning_content
//   2. message.reasoning
//   3. message.reasoning_details[].text  (OpenRouter style)
// Returns nullopt when the message has no reasoning attached.
std::optional<std::string> extract_reasoning_from_message(const nlohmann::json& message);

// Pull the user's task description out of a fresh conversation history.
// Returns the first non-empty `user` message's content (capped at 500 chars),
// or nullopt when no such message exists.  Used as context for tools that
// want a high-level goal hint (browser_snapshot etc.).
std::optional<std::string> extract_user_task(const nlohmann::json& messages, size_t max_len = 500);

// Normalise a tool_call (which may arrive as either an object-style entry or
// a vLLM dict entry) into the canonical OpenAI dict shape:
//   { id, type:"function", function:{name, arguments} }
// `id_fallback` is used when the input has no id field.
nlohmann::json tool_call_to_dict(const nlohmann::json& tc, const std::string& id_fallback);

// Convenience: read `function.name` regardless of which format the entry uses.
std::string tool_call_name(const nlohmann::json& tc);
// Convenience: read `function.arguments` (raw JSON string).
std::string tool_call_arguments(const nlohmann::json& tc);
// Convenience: read `id` from either format, using fallback when missing.
std::string tool_call_id(const nlohmann::json& tc, const std::string& fallback = "");

// Build the JSON-stringified error blob the Python loop appends as the
// tool result when the model invokes an unknown tool.  Mirrors:
//   {"error": "Unknown tool 'X'. Available tools: [...]"}
std::string format_unknown_tool_error(
    const std::string& tool_name,
    const std::unordered_set<std::string>& valid_tool_names);

// Build the JSON-stringified error blob the loop appends when the tool
// arguments string isn't valid JSON.
std::string format_invalid_json_error(const std::string& message);

// Inspect a tool-result string for an embedded error.  Returns true when the
// result is parseable JSON with non-empty `error` and a negative `exit_code`.
// Mirrors the post-execution recheck in the Python loop.
bool tool_result_has_negative_exit_error(const std::string& tool_result);

// True when the assistant message has no structured tool_calls but its content
// contains a raw `<tool_call>` block — the cue for the fallback parser path.
bool needs_fallback_tool_call_parse(const nlohmann::json& assistant_msg);

// Truncate to at most `max_len` chars.  No ellipsis (matches Python `[:max]`).
std::string truncate_to(const std::string& s, size_t max_len);

}  // namespace hermes::environments::agent_loop
