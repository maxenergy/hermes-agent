// MemoryProvider — pluggable memory backends. C++17 port of
// agent/memory_provider.py.
//
// Each provider produces a fragment that is concatenated into the system
// prompt, receives prefetch hints on every user message, and is asked to
// sync on every completed exchange. Providers also expose per-provider
// tools to the model and react to lifecycle events (session start/end,
// delegation to a subagent, pre-compression hooks, …).
//
// Built-in memory is always active as the first provider and cannot be
// removed. External providers (Honcho, Hindsight, Mem0, …) are additive.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/state/memory_store.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::agent {

// Context passed to initialize / on_turn_start / etc. Mirrors Python
// kwargs. Key/value strings only — providers that need richer payloads
// inspect the JSON "extras" field.
struct MemoryProviderContext {
    std::string session_id;
    std::string hermes_home;
    std::string platform;           // "cli", "telegram", ...
    std::string agent_context;      // "primary", "subagent", "cron", "flush"
    std::string agent_identity;     // profile name
    std::string agent_workspace;    // shared workspace name
    std::string parent_session_id;  // subagent parent
    std::string user_id;            // gateway user identifier
    nlohmann::json extras = nlohmann::json::object();
};

// Config schema entry for `hermes memory setup`.
struct MemoryConfigField {
    std::string key;
    std::string description;
    bool secret = false;
    bool required = false;
    std::string default_value;
    std::vector<std::string> choices;
    std::string url;
    std::string env_var;
};

class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;

    // -- Identity --
    virtual std::string name() const = 0;
    virtual bool is_external() const = 0;

    // -- Availability probe --
    // Must not issue network calls — check config and installed deps only.
    virtual bool is_available() const { return true; }

    // -- Session lifecycle --
    virtual void initialize(const MemoryProviderContext& ctx);
    virtual void shutdown();

    // -- Prompt injection --
    virtual std::string build_system_prompt_section() = 0;

    // -- Prefetch (sync API) --
    // Default returns empty; override for providers with ready-cached recalls.
    virtual std::string prefetch_string(const std::string& query,
                                        const std::string& session_id = "");

    // Back-compat fire-and-forget prefetch from the old cpp API.
    virtual void prefetch(std::string_view user_message) {
        (void)prefetch_string(std::string(user_message));
    }

    // Queue a background recall for the NEXT turn (default: no-op).
    virtual void queue_prefetch(const std::string& query,
                                const std::string& session_id = "");

    // -- Sync --
    virtual void sync(std::string_view user_msg,
                      std::string_view assistant_response);
    virtual void sync_turn(const std::string& user_content,
                           const std::string& assistant_content,
                           const std::string& session_id = "");

    // -- Tool exposure --
    virtual std::vector<nlohmann::json> get_tool_schemas() const;
    virtual std::string handle_tool_call(const std::string& tool_name,
                                         const nlohmann::json& args,
                                         const MemoryProviderContext& ctx);

    // -- Optional hooks --
    virtual void on_turn_start(int turn_number,
                               const std::string& user_message,
                               const MemoryProviderContext& ctx);
    virtual void on_session_end(
        const std::vector<hermes::llm::Message>& messages,
        const MemoryProviderContext& ctx);
    virtual std::string on_pre_compress(
        const std::vector<hermes::llm::Message>& messages);
    virtual void on_delegation(const std::string& task,
                               const std::string& result,
                               const std::string& child_session_id,
                               const MemoryProviderContext& ctx);
    virtual void on_memory_write(const std::string& action,
                                 const std::string& target,
                                 const std::string& content);

    // -- Setup UX --
    virtual std::vector<MemoryConfigField> get_config_schema() const;
    virtual void save_config(const nlohmann::json& values,
                             const std::string& hermes_home);
};

// Built-in provider backed by hermes::state::MemoryStore.
class BuiltinMemoryProvider : public MemoryProvider {
public:
    explicit BuiltinMemoryProvider(hermes::state::MemoryStore* store);

    std::string name() const override { return "builtin"; }
    bool is_external() const override { return false; }
    bool is_available() const override { return store_ != nullptr; }

    std::string build_system_prompt_section() override;
    void prefetch(std::string_view user_message) override;
    std::string prefetch_string(const std::string& query,
                                const std::string& session_id) override;
    void sync(std::string_view user_msg,
              std::string_view assistant_response) override;

private:
    hermes::state::MemoryStore* store_;
};

}  // namespace hermes::agent
