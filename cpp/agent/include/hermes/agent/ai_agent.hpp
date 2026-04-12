// AIAgent — the conversation loop that ties Phase 1-3 together.
//
// Responsibilities (mirror of Python run_agent.py's run_conversation):
//   1. Maintain the running message list, persisting to SessionDB.
//   2. Apply Anthropic prompt-cache markers before every API call.
//   3. Compress on demand via the injected ContextEngine.
//   4. Dispatch tool calls — intercepting `todo` and `memory` at the
//      agent level, forwarding everything else to the ToolDispatcher.
//   5. Emit lifecycle callbacks (assistant_message / tool_call /
//      tool_result / usage) for the gateway and CLI display layers.
//   6. Honour iteration budget + cooperative stop_requested().
//
// AIAgent does not construct its own dependencies — see the constructor.
#pragma once

#include "hermes/agent/context_engine.hpp"
#include "hermes/agent/iteration_budget.hpp"
#include "hermes/agent/memory_manager.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/usage.hpp"
#include "hermes/state/memory_store.hpp"
#include "hermes/state/session_db.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::agent {

// Tool dispatcher signature.  Phase 5 (Agent ε) wires the real
// registry; for Phase 4 we accept any callable.
using ToolDispatcher = std::function<std::string(
    const std::string& name,
    const nlohmann::json& args,
    const std::string& task_id)>;

struct AgentCallbacks {
    std::function<void(const hermes::llm::Message&)> on_assistant_message;
    std::function<void(const std::string&, const nlohmann::json&)> on_tool_call;
    std::function<void(const std::string&, const std::string&)> on_tool_result;
    std::function<void(int64_t, int64_t, double)> on_usage;
};

struct AgentConfig {
    std::string model = "anthropic/claude-opus-4-6";
    int max_iterations = 90;
    std::vector<std::string> enabled_toolsets;
    std::vector<std::string> disabled_toolsets;
    bool quiet_mode = false;
    bool save_trajectories = false;
    std::string platform = "cli";
    std::string session_id;        // empty → AIAgent calls create_session
    bool skip_context_files = false;
    bool skip_memory = false;
    std::string provider;          // "openai" | "anthropic" | "openrouter"
    std::string api_mode = "openai";
    double temperature = 1.0;
    int reasoning_effort = 0;
    int64_t max_context_tokens = 200'000;
    int max_retries = 2;
    nlohmann::json extra;
};

struct ConversationResult {
    std::string final_response;
    std::vector<hermes::llm::Message> messages;
    hermes::llm::CanonicalUsage usage;
    int iterations_used = 0;
    bool completed = true;
    std::optional<std::string> error;
};

class AIAgent {
public:
    AIAgent(AgentConfig config,
            hermes::llm::LlmClient* llm,
            hermes::state::SessionDB* session_db,
            ContextEngine* context_engine,
            MemoryManager* memory,
            PromptBuilder* prompt_builder,
            ToolDispatcher tool_dispatcher,
            std::vector<hermes::llm::ToolSchema> tool_schemas = {},
            AgentCallbacks callbacks = {});

    ~AIAgent();

    AIAgent(const AIAgent&) = delete;
    AIAgent& operator=(const AIAgent&) = delete;

    // Inject a non-owning MemoryStore for the agent-level `memory`
    // tool.  May be nullptr (the tool then returns an error string).
    void set_memory_store(hermes::state::MemoryStore* store);

    // Single-call helper used by simple integrations (e.g. unit
    // tests).  Returns the final assistant text — equivalent to
    // run_conversation(message).final_response.
    std::string chat(const std::string& message);

    // Full loop entry point.  See ConversationResult for the returned
    // fields.  `system_message_override` lets the caller pin a custom
    // system prompt; otherwise PromptBuilder is invoked.
    // `conversation_history` lets the caller resume from a saved log.
    ConversationResult run_conversation(
        const std::string& user_message,
        std::optional<std::string> system_message_override = std::nullopt,
        std::optional<std::vector<hermes::llm::Message>> conversation_history = std::nullopt,
        const std::string& task_id = "");

    void request_stop();
    bool stop_requested() const;

    const AgentConfig& config() const;
    const std::vector<hermes::llm::Message>& messages() const;
    const hermes::llm::CanonicalUsage& total_usage() const;

    // Test hook: allow tests to swap in a no-op sleep so backoff
    // retries don't actually wait.
    void set_sleep_function(std::function<void(std::chrono::milliseconds)> fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hermes::agent
