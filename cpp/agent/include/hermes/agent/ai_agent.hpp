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
#include "hermes/agent/rate_limit_tracker.hpp"
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
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::agent {

// Tool dispatcher signature.  Phase 5 (Agent ε) wires the real
// registry; for Phase 4 we accept any callable.
using ToolDispatcher = std::function<std::string(
    const std::string& name,
    const nlohmann::json& args,
    const std::string& task_id)>;

// Port of run_agent.py::AIAgent._emit_status — lifecycle messages
// (e.g. "Rate limited — switching to fallback", "Context too large —
// compressing").  The phase identifies the originating call site so the
// gateway / CLI can render accordingly; the message is free-form and
// already formatted for display.
using StatusCallback =
    std::function<void(std::string_view phase, std::string_view message)>;

// Port of run_agent.py::AIAgent._emit_context_pressure — notifies the
// consumer that context is approaching the compaction threshold.
// compaction_progress is in the range [0.0, 1.0] (1.0 = firing).
using ContextPressureCallback = std::function<void(
    double compaction_progress,
    std::int64_t threshold_tokens,
    double threshold_percent,
    bool compression_enabled)>;

// Port of run_agent.py::AIAgent._emit_telemetry — generic event
// sink for instrumentation.  Payload is an opaque JSON object.
using TelemetryCallback =
    std::function<void(std::string_view event, const nlohmann::json& payload)>;

// Port of run_agent.py::AIAgent._persist_user_message_override — allows
// the caller to rewrite the just-appended user message before it is
// persisted to SessionDB, without letting the rewritten text leak into
// the on-wire API request.  Returning std::nullopt leaves the original
// message unchanged.
using PersistUserMessageOverride =
    std::function<std::optional<std::string>(const std::string& original)>;

// Port of run_agent.py::AIAgent tool_progress_callback — fired around
// every tool dispatch (start/end/error) so REPL-style consumers can
// render inline progress ("🔧 tool_name: arg preview").  The
// arguments_preview is truncated to 120 chars; phase is one of
// "start" / "end" / "error".  Suppressed when quiet_mode is true.
using ToolProgressCallback = std::function<void(
    std::string_view tool_name,
    std::string_view arguments_preview,
    std::string_view phase)>;

struct AgentCallbacks {
    std::function<void(const hermes::llm::Message&)> on_assistant_message;
    std::function<void(const std::string&, const nlohmann::json&)> on_tool_call;
    std::function<void(const std::string&, const std::string&)> on_tool_result;
    std::function<void(int64_t, int64_t, double)> on_usage;

    // Phase 4 observability/correctness callbacks (all optional; default
    // to no-op).  Defining these as members of AgentCallbacks keeps the
    // constructor signature unchanged.
    StatusCallback on_status;
    ContextPressureCallback on_context_pressure;
    TelemetryCallback on_telemetry;
    PersistUserMessageOverride persist_user_message_override;
};

// Activity summary returned by AIAgent::activity_summary() — mirrors
// run_agent.py::AIAgent.get_activity_summary().  Used by the gateway
// timeout handler and the "still working" heartbeat.
struct AgentActivitySummary {
    double last_activity_ts = 0.0;
    std::string last_activity_desc;
    double seconds_since_activity = 0.0;
    std::string current_tool;
    int api_call_count = 0;
    int max_iterations = 0;
    int budget_used = 0;
    int budget_max = 0;
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
    // Any /steer text still pending when the agent exited (no further
    // tool batch to drain into).  Callers that surface this — CLI /
    // gateway — should deliver it as the next user turn instead of
    // silently dropping it.  Empty when no leftover.  See
    // AIAgent::steer for the full contract.  Ports upstream commit
    // 2edebedc.
    std::string pending_steer;
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

    // ── /steer: mid-run injection without interrupt ──────────────────
    //
    // Port of run_agent.py::AIAgent.steer (upstream commit 2edebedc).
    // Unlike request_stop(), this does NOT stop the current tool call.
    // The text is stashed; once the current tool batch finishes, the
    // agent loop appends it to the LAST ``role:"tool"`` message's
    // content.  The model sees the steer as part of that tool's
    // output on its next iteration — no new user turn, no role
    // alternation violation, no prompt-cache churn beyond the normal
    // per-turn tool-result update.
    //
    // Thread-safe: callable from any thread (gateway, CLI, TUI).
    // Multiple calls before the drain point concatenate with '\n'.
    //
    // Returns true on accept, false on empty payload.  Empty/whitespace
    // inputs are rejected without modifying state.
    bool steer(const std::string& text);

    // Returns the currently-stashed /steer text (if any) and clears
    // the slot atomically.  Visible for gateway integrations that
    // want to deliver a leftover steer as the next user turn when the
    // agent exits before draining it; prefer calling steer() for
    // normal injection flows.
    std::string drain_pending_steer();

    const AgentConfig& config() const;
    const std::vector<hermes::llm::Message>& messages() const;
    const hermes::llm::CanonicalUsage& total_usage() const;

    // Test hook: allow tests to swap in a no-op sleep so backoff
    // retries don't actually wait.
    void set_sleep_function(std::function<void(std::chrono::milliseconds)> fn);

    // ── Observability / diagnostics ──────────────────────────────────
    //
    // Port of run_agent.py::AIAgent.get_activity_summary — snapshot of
    // what the agent is currently doing.  Thread-safe with respect to
    // the running loop (values are atomics / single-reader copies).
    AgentActivitySummary activity_summary() const;

    // Port of run_agent.py::AIAgent.get_rate_limit_state — returns the
    // most recently captured RateLimitState (after the last successful
    // API call), or std::nullopt if none has been captured.
    std::optional<RateLimitState> rate_limit_state() const;

    // Port of run_agent.py::AIAgent._capture_rate_limits — feed a
    // provider HTTP response's header map and provider name to update
    // the internal RateLimitState snapshot.  Typically invoked by the
    // LLM client after each request; exposed here so tests and gateway
    // integrations can drive it directly.
    void capture_rate_limits(
        const std::unordered_map<std::string, std::string>& headers,
        std::string_view provider);

    // Port of run_agent.py::AIAgent._invalidate_system_prompt — drops
    // any cached system prompt so the next run_conversation() rebuilds
    // it from the current memory snapshot.  Called by the loop after
    // compression events; exposed for callers that rewrite memory out
    // of band and want the next turn to pick it up.  Note: the current
    // C++ loop does not cache the system prompt between turns, so this
    // is a no-op today — it exists so call sites can express intent
    // symmetrically with Python.
    void invalidate_system_prompt();

    // Runtime mutators for the observability callbacks.  All tolerate
    // empty functors (treated as "no callback").
    void set_status_callback(StatusCallback cb);
    void set_context_pressure_callback(ContextPressureCallback cb);
    void set_telemetry_callback(TelemetryCallback cb);
    void set_persist_user_message_override(PersistUserMessageOverride cb);

    // ── Quiet mode + tool progress ───────────────────────────────────
    //
    // Port of run_agent.py::AIAgent.quiet_mode + tool_progress_callback.
    // When quiet_mode is true: tool_progress_callback AND the existing
    // on_tool_call/on_tool_result callbacks are suppressed so the REPL
    // can offer a headless mode.  Default quiet_mode=false preserves
    // the existing behaviour.
    void set_tool_progress_callback(ToolProgressCallback cb);
    void set_quiet_mode(bool q);
    bool quiet_mode() const;

    // ── Background task pool ─────────────────────────────────────────
    //
    // Port of run_agent.py::AIAgent._spawn_background_review — submit a
    // task (insights / trajectory review / memory GC) to a worker pool
    // that is owned by this agent.  The pool is joined at agent
    // destruction, so callers need not manage thread lifetime.  Safe
    // to call from any thread.
    void spawn_background_review(std::function<void()> fn);

    // Test-only accessor for the background pool's pending count.
    std::size_t background_pending() const;

    // Block until all currently-submitted background tasks have
    // finished.  Mainly useful in tests.
    void wait_background_idle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hermes::agent
