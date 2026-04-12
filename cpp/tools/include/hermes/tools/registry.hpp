// Central registry for all hermes tools — schemas, handlers, and dispatch.
//
// Mirrors the semantics of the Python ``tools/registry.py`` module:
//
//   * Each tool registers a JSON schema (OpenAI function-call shape), a
//     C++ handler, an optional availability check, and a list of required
//     environment variables.
//   * dispatch() looks the tool up by name, runs the handler in a try
//     block, applies result-size truncation, and always returns a
//     JSON-encoded string.
//   * get_definitions() builds the ``ToolSchema`` list passed to the LLM
//     client, filtered by toolset / availability.
//
// The registry is process-global by design — tool modules call register()
// at startup and the AIAgent queries it at request build time.
#pragma once

#include "hermes/llm/llm_client.hpp"
#include "hermes/tools/budget_config.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::tools {

// Per-call context handed to every tool handler.  Tools that need to know
// about the active session, the current platform (cli / telegram / ...),
// or the working directory pull it from here instead of from globals.
struct ToolContext {
    std::string task_id;
    std::string user_task;
    std::string session_key;
    std::string platform;  // "cli", "telegram", "discord", ...
    std::string cwd;
    nlohmann::json extra;  // arbitrary provider-specific kwargs
};

// Handler signature.  Returns a JSON-encoded string.  Exceptions thrown by
// handlers are caught by the registry and converted to ``{"error": "..."}``;
// returning an empty / null string is converted to ``{"ok": true}``.
using ToolHandler = std::function<std::string(const nlohmann::json& args,
                                              const ToolContext& ctx)>;

// Availability check.  May be called many times per dispatch — keep it
// cheap.  A check that throws is treated as ``false``.
using CheckFn = std::function<bool()>;

// Metadata for a single registered tool.
struct ToolEntry {
    std::string name;
    std::string toolset;
    nlohmann::json schema;  // OpenAI function-call schema
    ToolHandler handler;
    CheckFn check_fn;
    std::vector<std::string> requires_env;
    bool is_async = false;
    std::string description;
    std::string emoji;
    // 0 means "use DEFAULT_RESULT_SIZE_CHARS".
    std::size_t max_result_size_chars = 0;
};

class ToolRegistry {
public:
    // Process-wide singleton.
    static ToolRegistry& instance();

    // -- Registration ---------------------------------------------------

    void register_tool(ToolEntry entry);
    void register_toolset_check(std::string toolset, CheckFn fn);
    bool deregister(const std::string& name);

    // -- Dispatch -------------------------------------------------------

    // Run a tool by name.  Always returns a JSON-encoded string; never
    // throws.  See registry.cpp for the precise error envelopes.
    std::string dispatch(const std::string& name,
                         const nlohmann::json& args,
                         const ToolContext& ctx);

    // -- Schema introspection ------------------------------------------

    // Returns all schemas whose tool's toolset is in ``enabled`` (or all,
    // if ``enabled`` is empty) AND not in ``disabled`` AND passes both the
    // tool's check_fn and its toolset's check.
    std::vector<hermes::llm::ToolSchema> get_definitions(
        const std::vector<std::string>& enabled = {},
        const std::vector<std::string>& disabled = {}) const;

    // -- Query helpers --------------------------------------------------

    std::optional<std::string> get_toolset_for_tool(const std::string& name) const;
    std::vector<std::string> list_tools() const;
    std::vector<std::string> list_toolsets() const;
    bool has_tool(const std::string& name) const;
    std::string get_emoji(const std::string& tool_name) const;
    std::size_t get_max_result_size(const std::string& tool_name) const;
    bool is_toolset_available(const std::string& toolset) const;

    // Last-resolved tool names — process-global state used by delegate
    // subagents to save/restore the active tool list across nested
    // dispatches.  See AGENTS.md pitfall #4.
    std::vector<std::string> last_resolved_tool_names() const;
    void set_last_resolved_tool_names(std::vector<std::string> names);

    // -- Testing helpers ------------------------------------------------

    // Wipe everything.  NOT thread-safe with concurrent dispatch().
    void clear();
    std::size_t size() const;

private:
    ToolRegistry() = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    mutable std::mutex mu_;
    std::unordered_map<std::string, ToolEntry> tools_;
    std::unordered_map<std::string, CheckFn> toolset_checks_;
    std::vector<std::string> last_resolved_;
};

// ---------------------------------------------------------------------------
// Helpers for tool handlers — produce a consistent JSON envelope without
// boilerplate ``nlohmann::json`` construction.
// ---------------------------------------------------------------------------

// Return ``data`` serialized as JSON.  If ``data`` is already an object it
// is emitted as-is; scalars / arrays are wrapped in ``{"output": ...}``.
std::string tool_result(const nlohmann::json& data);

// Convenience overload for handlers that want to emit a literal object
// from key/value pairs.
std::string tool_result(
    std::initializer_list<std::pair<std::string, nlohmann::json>> kv);

// Build an error envelope.  ``extra`` keys are merged onto
// ``{"error": message}`` so callers can include things like
// ``{"code", 404}`` without an extra json() construction.
std::string tool_error(
    std::string_view message,
    std::initializer_list<std::pair<std::string, nlohmann::json>> extra = {});

}  // namespace hermes::tools
