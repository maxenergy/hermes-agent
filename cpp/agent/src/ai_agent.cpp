#include "hermes/agent/ai_agent.hpp"

#include "hermes/agent/rate_limit_tracker.hpp"
#include "hermes/core/path.hpp"
#include "hermes/llm/error_classifier.hpp"
#include "hermes/llm/model_metadata.hpp"
#include "hermes/llm/prompt_cache.hpp"
#include "hermes/llm/retry_policy.hpp"
#include "hermes/state/trajectory.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hermes::agent {

namespace {

using hermes::llm::ApiError;
using hermes::llm::CanonicalUsage;
using hermes::llm::ClassifiedError;
using hermes::llm::CompletionRequest;
using hermes::llm::CompletionResponse;
using hermes::llm::FailoverReason;
using hermes::llm::Message;
using hermes::llm::Role;
using hermes::llm::ToolCall;
using hermes::llm::ToolSchema;

void accumulate_usage(CanonicalUsage& acc, const CanonicalUsage& add) {
    acc.input_tokens += add.input_tokens;
    acc.output_tokens += add.output_tokens;
    acc.cache_read_input_tokens += add.cache_read_input_tokens;
    acc.cache_creation_input_tokens += add.cache_creation_input_tokens;
    acc.reasoning_tokens += add.reasoning_tokens;
}

Message make_tool_result_message(const std::string& tool_call_id,
                                 const std::string& result_text) {
    Message m;
    m.role = Role::Tool;
    m.tool_call_id = tool_call_id;
    m.content_text = result_text;
    return m;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────
// Implementation
// ──────────────────────────────────────────────────────────────────────

struct AIAgent::Impl {
    AgentConfig config;
    hermes::llm::LlmClient* llm;
    hermes::state::SessionDB* session_db;
    ContextEngine* context_engine;
    MemoryManager* memory;
    PromptBuilder* prompt_builder;
    ToolDispatcher tool_dispatcher;
    std::vector<ToolSchema> tool_schemas;
    AgentCallbacks callbacks;

    std::vector<Message> messages;
    CanonicalUsage total_usage{};
    std::atomic<bool> stop_requested{false};
    hermes::state::MemoryStore* memory_store = nullptr;
    std::function<void(std::chrono::milliseconds)> sleep_fn;

    // Agent-level todo state — opaque key/value list of items.
    nlohmann::json todos = nlohmann::json::array();

    // ── Observability state ───────────────────────────────────────────
    // Last-activity tracking for the gateway timeout + heartbeat.
    // Values are written from the loop thread and read from a secondary
    // thread (the timeout watcher), so we gate them on a mutex.  The
    // writes happen at coarse granularity (per iteration / tool call)
    // and reads happen at most every few seconds, so contention is
    // negligible.
    mutable std::mutex activity_mutex;
    double last_activity_ts = 0.0;
    std::string last_activity_desc;
    std::string current_tool;
    int api_call_count = 0;

    // Captured rate-limit state from the most recent API call.
    mutable std::mutex rl_mutex;
    std::optional<RateLimitState> rate_limit_state;

    // True after any code path has called invalidate_system_prompt().
    // Today the C++ loop rebuilds the prompt every turn, so this is
    // advisory only — it is exposed for tests and future caching work.
    std::atomic<bool> system_prompt_invalidated{false};

    static double now_seconds() {
        using namespace std::chrono;
        return duration_cast<duration<double>>(
                   system_clock::now().time_since_epoch())
            .count();
    }

    void touch_activity(std::string desc) {
        std::lock_guard<std::mutex> lk(activity_mutex);
        last_activity_ts = now_seconds();
        last_activity_desc = std::move(desc);
    }

    void set_current_tool(std::string name) {
        std::lock_guard<std::mutex> lk(activity_mutex);
        current_tool = std::move(name);
    }

    // ── Callback emitters ────────────────────────────────────────────
    void emit_status(std::string_view phase, std::string_view msg) {
        if (!callbacks.on_status) return;
        try { callbacks.on_status(phase, msg); } catch (...) {}
    }

    void emit_context_pressure(double progress,
                               std::int64_t threshold_tokens,
                               double threshold_pct,
                               bool compression_enabled) {
        if (!callbacks.on_context_pressure) return;
        try {
            callbacks.on_context_pressure(progress, threshold_tokens,
                                          threshold_pct, compression_enabled);
        } catch (...) {}
    }

    void emit_telemetry(std::string_view event, const nlohmann::json& payload) {
        if (!callbacks.on_telemetry) return;
        try { callbacks.on_telemetry(event, payload); } catch (...) {}
    }

    // Port of run_agent.py::AIAgent._apply_persist_user_message_override.
    // Rewrites the most recently appended user message (if any) in place
    // before it is persisted.  Returns true iff a rewrite happened.
    bool apply_persist_user_message_override() {
        if (!callbacks.persist_user_message_override) return false;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == Role::User) {
                try {
                    auto replacement =
                        callbacks.persist_user_message_override(
                            it->content_text);
                    if (replacement.has_value()) {
                        it->content_text = std::move(*replacement);
                        return true;
                    }
                } catch (...) {
                    // Swallow — never let an override hook break the loop.
                }
                return false;
            }
        }
        return false;
    }

    void persist_message(const Message& m, int turn_index) {
        if (!session_db || config.session_id.empty()) return;
        try {
            hermes::state::MessageRow row;
            row.session_id = config.session_id;
            row.turn_index = turn_index;
            row.role = hermes::llm::role_to_string(m.role);
            row.content = m.content_text;
            row.tool_calls = nlohmann::json::array();
            for (const auto& tc : m.tool_calls) {
                row.tool_calls.push_back(
                    nlohmann::json{{"id", tc.id},
                                   {"name", tc.name},
                                   {"arguments", tc.arguments}});
            }
            if (m.reasoning) row.reasoning = *m.reasoning;
            row.created_at = std::chrono::system_clock::now();
            session_db->save_message(row);
        } catch (...) {
            // SessionDB failures must not abort the loop.
        }
    }

    void ensure_session() {
        if (!session_db || !config.session_id.empty()) return;
        try {
            nlohmann::json cfg;
            cfg["model"] = config.model;
            cfg["platform"] = config.platform;
            config.session_id =
                session_db->create_session(config.platform, config.model, cfg);
        } catch (...) {
            // Tests may run without a session DB; we tolerate creation
            // failures and continue with an empty session_id.
        }
    }

    void emit_assistant(const Message& m) {
        if (callbacks.on_assistant_message) {
            try { callbacks.on_assistant_message(m); } catch (...) {}
        }
    }
    void emit_tool_call(const std::string& name, const nlohmann::json& args) {
        if (callbacks.on_tool_call) {
            try { callbacks.on_tool_call(name, args); } catch (...) {}
        }
    }
    void emit_tool_result(const std::string& name, const std::string& result) {
        if (callbacks.on_tool_result) {
            try { callbacks.on_tool_result(name, result); } catch (...) {}
        }
    }
    void emit_usage(const CanonicalUsage& usage) {
        if (!callbacks.on_usage) return;
        try {
            callbacks.on_usage(usage.input_tokens, usage.output_tokens, 0.0);
        } catch (...) {}
    }

    // ── Agent-level tool intercept ────────────────────────────────────
    std::string handle_todo(const nlohmann::json& args) {
        const bool merge = args.value("merge", false);
        if (args.contains("todos")) {
            if (!args["todos"].is_array()) {
                return R"({"error":"todos must be an array"})";
            }
            if (!merge) {
                todos = args["todos"];
            } else {
                // Merge by id.
                for (const auto& incoming : args["todos"]) {
                    if (!incoming.contains("id")) continue;
                    bool replaced = false;
                    for (auto& existing : todos) {
                        if (existing.value("id", "") == incoming.value("id", "")) {
                            for (auto it = incoming.begin(); it != incoming.end(); ++it) {
                                existing[it.key()] = it.value();
                            }
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) todos.push_back(incoming);
                }
            }
        }
        nlohmann::json out;
        out["todos"] = todos;
        out["total"] = todos.size();
        return out.dump();
    }

    std::string handle_memory(const nlohmann::json& args) {
        if (!memory_store) {
            return R"({"error":"memory store not configured"})";
        }
        const std::string action = args.value("action", "");
        // Accept both `file` (schema in cpp/tools/src/memory_tool.cpp)
        // and `scope` (legacy alias some callers still emit).  Both
        // resolve to MemoryFile::{Agent,User}.
        std::string target = args.value("file", std::string{});
        if (target.empty()) target = args.value("scope", "agent");
        const auto file = target == "user"
                              ? hermes::state::MemoryFile::User
                              : hermes::state::MemoryFile::Agent;
        try {
            if (action == "add") {
                // Schema param is `entry`; accept legacy `content` too.
                std::string entry = args.value("entry", std::string{});
                if (entry.empty()) entry = args.value("content", "");
                memory_store->add(file, entry);
                return R"({"ok":true,"action":"add"})";
            }
            if (action == "read") {
                auto entries = memory_store->read_all(file);
                return nlohmann::json{{"entries", entries}}.dump();
            }
            if (action == "replace") {
                memory_store->replace(file,
                                      args.value("needle", ""),
                                      args.value("replacement", ""));
                return R"({"ok":true,"action":"replace"})";
            }
            if (action == "remove") {
                memory_store->remove(file, args.value("needle", ""));
                return R"({"ok":true,"action":"remove"})";
            }
            return R"({"error":"unknown memory action"})";
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = e.what();
            return err.dump();
        }
    }

    bool is_agent_level_tool(const std::string& name) const {
        return name == "todo" || name == "memory";
    }

    std::string dispatch_tool(const ToolCall& tc, const std::string& task_id) {
        if (is_agent_level_tool(tc.name)) {
            if (tc.name == "todo") return handle_todo(tc.arguments);
            return handle_memory(tc.arguments);
        }
        if (!tool_dispatcher) {
            return R"({"error":"no tool dispatcher configured"})";
        }
        try {
            return tool_dispatcher(tc.name, tc.arguments, task_id);
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["error"] = e.what();
            return err.dump();
        }
    }

    void do_sleep(std::chrono::milliseconds d) {
        if (sleep_fn) {
            sleep_fn(d);
            return;
        }
        std::this_thread::sleep_for(d);
    }

    void save_trajectory(const ConversationResult& result) {
        if (!config.save_trajectories) return;
        try {
            auto traj_dir = hermes::core::path::get_hermes_home() / "trajectories";
            std::string filename = config.session_id.empty()
                                       ? "trajectory_samples.jsonl"
                                       : (config.session_id + ".jsonl");
            hermes::state::TrajectoryWriter writer(traj_dir / filename);

            // Convert messages to JSON array.
            nlohmann::json msgs_json = nlohmann::json::array();
            for (const auto& m : result.messages) {
                nlohmann::json mj;
                mj["role"] = hermes::llm::role_to_string(m.role);
                mj["content"] = m.content_text;
                if (!m.tool_calls.empty()) {
                    mj["tool_calls"] = nlohmann::json::array();
                    for (const auto& tc : m.tool_calls) {
                        mj["tool_calls"].push_back(
                            nlohmann::json{{"id", tc.id},
                                           {"name", tc.name},
                                           {"arguments", tc.arguments}});
                    }
                }
                if (m.reasoning) mj["reasoning"] = *m.reasoning;
                msgs_json.push_back(std::move(mj));
            }

            hermes::state::TrajectoryRecord rec;
            rec.model = config.model;
            rec.messages = std::move(msgs_json);
            rec.completed = result.completed;
            rec.error = result.error;
            writer.write(rec);
        } catch (...) {
            // Trajectory saving must not abort the agent.
        }
    }

    // ── The core loop ─────────────────────────────────────────────────
    ConversationResult run(std::string user_message,
                           std::optional<std::string> system_override,
                           std::vector<Message> history,
                           const std::string& task_id) {
        ConversationResult result;

        if (stop_requested.load()) {
            result.completed = false;
            result.error = "stop requested before first iteration";
            result.iterations_used = 0;
            return result;
        }

        ensure_session();

        // Seed the message list — caller may have supplied a history,
        // otherwise build the system prompt from scratch.
        messages = std::move(history);
        if (messages.empty() ||
            (messages.front().role != Role::System && system_override)) {
            std::string sys_text;
            if (system_override) {
                sys_text = *system_override;
            } else if (prompt_builder) {
                PromptContext ctx;
                ctx.platform = config.platform;
                ctx.model = config.model;
                // Snapshot MEMORY.md / USER.md into the system prompt
                // so the model sees recalled facts at session start.
                // Mid-session writes via the `memory` tool update the
                // files on disk but intentionally do NOT mutate this
                // snapshot — that preserves the prompt cache; the
                // refreshed snapshot arrives with the next session.
                if (memory_store) {
                    try {
                        ctx.memory_entries = memory_store->read_all(
                            hermes::state::MemoryFile::Agent);
                        ctx.user_entries = memory_store->read_all(
                            hermes::state::MemoryFile::User);
                    } catch (...) { /* swallow — memory is best-effort */ }
                }
                sys_text = prompt_builder->build_system_prompt(ctx);
            }
            if (!sys_text.empty()) {
                Message sys;
                sys.role = Role::System;
                sys.content_text = std::move(sys_text);
                messages.insert(messages.begin(), std::move(sys));
            }
        }

        // Append the user turn.
        if (!user_message.empty()) {
            Message u;
            u.role = Role::User;
            u.content_text = std::move(user_message);
            messages.push_back(std::move(u));
            // Apply the persist-override hook before saving — mirrors
            // run_agent.py::_apply_persist_user_message_override being
            // called inside _persist_session / _flush_messages_to_session_db.
            apply_persist_user_message_override();
            persist_message(messages.back(),
                            static_cast<int>(messages.size()) - 1);
        }

        touch_activity("run_conversation: loop started");
        emit_status("start", "run_conversation entered");
        emit_telemetry("run_conversation_start",
                       nlohmann::json{{"model", config.model},
                                      {"session_id", config.session_id}});

        IterationBudget budget(config.max_iterations);
        int api_calls = 0;

        while (api_calls < config.max_iterations && !budget.exhausted()) {
            if (stop_requested.load()) {
                result.completed = false;
                result.error = "stop requested";
                break;
            }

            // 1. Compress on demand.
            if (context_engine) {
                int64_t cur =
                    hermes::llm::estimate_messages_tokens_rough(messages);
                // Emit context-pressure when we are above 70% of the
                // budget — mirrors Python's _emit_context_pressure which
                // fires as compaction approaches.
                if (config.max_context_tokens > 0) {
                    double pct =
                        static_cast<double>(cur) /
                        static_cast<double>(config.max_context_tokens);
                    if (pct >= 0.70) {
                        emit_context_pressure(
                            pct,
                            config.max_context_tokens,
                            0.70,
                            /*compression_enabled=*/true);
                    }
                }
                std::size_t before = messages.size();
                messages = context_engine->compress(
                    std::move(messages), cur, config.max_context_tokens);
                if (messages.size() < before) {
                    // Compression fired — invalidate the cached system
                    // prompt (no-op today, advisory) and tell consumers.
                    system_prompt_invalidated.store(true);
                    emit_status("compress",
                                "context compressed to fit budget");
                    emit_telemetry(
                        "context_compressed",
                        nlohmann::json{
                            {"before_messages", before},
                            {"after_messages", messages.size()},
                            {"token_estimate", cur},
                            {"budget", config.max_context_tokens}});
                }
            }

            // 2. Apply prompt-cache markers (only meaningful for
            //    Anthropic native).
            hermes::llm::PromptCacheOptions cache_opts;
            cache_opts.native_anthropic = (config.provider == "anthropic");
            hermes::llm::apply_anthropic_cache_control(messages, cache_opts);

            // 3. Build request.
            CompletionRequest req;
            req.model = config.model;
            req.messages = messages;
            req.tools = tool_schemas;
            req.temperature = config.temperature;
            req.reasoning_effort = config.reasoning_effort;
            req.cache = cache_opts;
            req.extra = config.extra;

            // 4. Call the LLM with retry-on-classification.
            CompletionResponse resp;
            bool got_response = false;
            int attempt = 0;
            bool retried_compression = false;
            while (!got_response && attempt <= config.max_retries) {
                ++attempt;
                try {
                    resp = llm->complete(req);
                    got_response = true;
                } catch (const ApiError& e) {
                    auto cls = hermes::llm::classify_api_error(
                        e.status, e.body);
                    if (cls.reason == FailoverReason::ContextOverflow &&
                        !retried_compression && context_engine) {
                        // Force a compression pass and retry.
                        int64_t budget_tokens = cls.context_limit_hint
                                                    ? *cls.context_limit_hint
                                                    : config.max_context_tokens;
                        emit_status(
                            "compress",
                            "context overflow — forcing compression retry");
                        messages = context_engine->compress(
                            std::move(messages), budget_tokens * 2,
                            budget_tokens);
                        req.messages = messages;
                        retried_compression = true;
                        system_prompt_invalidated.store(true);
                        continue;
                    }
                    if (cls.reason == FailoverReason::RateLimit) {
                        emit_status("rate_limit",
                                    "rate limited — backing off");
                        emit_telemetry(
                            "retry",
                            nlohmann::json{{"reason", "rate_limit"},
                                           {"attempt", attempt}});
                        do_sleep(hermes::llm::backoff_for_error(attempt, cls));
                        continue;
                    }
                    if (cls.reason == FailoverReason::ServerError ||
                        cls.reason == FailoverReason::Timeout ||
                        cls.reason == FailoverReason::NetworkError) {
                        emit_status("retry",
                                    "transient API error — retrying");
                        emit_telemetry(
                            "retry",
                            nlohmann::json{{"reason", "transient"},
                                           {"attempt", attempt}});
                        do_sleep(hermes::llm::backoff_for_error(attempt, cls));
                        continue;
                    }
                    // Auth / unknown / model unavailable: fatal.
                    emit_status("error",
                                std::string("fatal API error: ") + e.what());
                    result.completed = false;
                    result.error = std::string("API error: ") + e.what();
                    result.iterations_used = api_calls;
                    result.usage = total_usage;
                    result.messages = messages;
                    return result;
                } catch (const std::exception& e) {
                    result.completed = false;
                    result.error = std::string("LLM call failed: ") + e.what();
                    result.iterations_used = api_calls;
                    result.usage = total_usage;
                    result.messages = messages;
                    return result;
                }
            }
            if (!got_response) {
                result.completed = false;
                result.error = "exceeded max retries";
                break;
            }

            // 5. Record assistant turn.
            messages.push_back(resp.assistant_message);
            persist_message(resp.assistant_message,
                            static_cast<int>(messages.size()) - 1);
            accumulate_usage(total_usage, resp.usage);
            emit_assistant(resp.assistant_message);
            emit_usage(resp.usage);

            // 6. Terminate if no tool calls.
            if (resp.assistant_message.tool_calls.empty()) {
                result.final_response = resp.assistant_message.content_text;
                if (result.final_response.empty()) {
                    for (const auto& b : resp.assistant_message.content_blocks) {
                        if (b.type == "text") {
                            result.final_response = b.text;
                            break;
                        }
                    }
                }
                // Thinking-model fallback: some models (qwen3.6-plus) return
                // only reasoning_content with empty content when they decide
                // the reasoning IS the answer. Surface it so we don't emit
                // an empty response.
                if (result.final_response.empty() &&
                    resp.assistant_message.reasoning &&
                    !resp.assistant_message.reasoning->empty()) {
                    result.final_response = *resp.assistant_message.reasoning;
                }
                result.completed = true;
                result.iterations_used = api_calls + 1;
                result.usage = total_usage;
                result.messages = messages;
                emit_status("done", "final response produced");
                emit_telemetry(
                    "run_conversation_done",
                    nlohmann::json{
                        {"iterations", result.iterations_used},
                        {"input_tokens", total_usage.input_tokens},
                        {"output_tokens", total_usage.output_tokens}});
                save_trajectory(result);
                return result;
            }

            // 7. Dispatch tool calls.
            for (const auto& tc : resp.assistant_message.tool_calls) {
                if (stop_requested.load()) break;
                emit_tool_call(tc.name, tc.arguments);
                set_current_tool(tc.name);
                touch_activity("running tool: " + tc.name);
                std::string tool_result = dispatch_tool(tc, task_id);
                touch_activity("tool completed: " + tc.name);
                set_current_tool("");
                Message tr = make_tool_result_message(tc.id, tool_result);
                emit_tool_result(tc.name, tool_result);
                persist_message(tr, static_cast<int>(messages.size()));
                messages.push_back(std::move(tr));
            }

            ++api_calls;
            {
                std::lock_guard<std::mutex> lk(activity_mutex);
                api_call_count = api_calls;
            }
            budget.consume(1);
        }

        // Loop exited without a final assistant text — return whatever
        // we have.
        result.completed = false;
        if (!result.error) {
            result.error = "iteration budget exhausted";
        }
        result.iterations_used = api_calls;
        result.usage = total_usage;
        result.messages = messages;
        // Provide last assistant text if any.
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == Role::Assistant) {
                if (!it->content_text.empty()) {
                    result.final_response = it->content_text;
                } else {
                    for (const auto& b : it->content_blocks) {
                        if (b.type == "text") {
                            result.final_response = b.text;
                            break;
                        }
                    }
                }
                break;
            }
        }
        save_trajectory(result);
        return result;
    }
};

// ──────────────────────────────────────────────────────────────────────
// Public surface
// ──────────────────────────────────────────────────────────────────────

AIAgent::AIAgent(AgentConfig config,
                 hermes::llm::LlmClient* llm,
                 hermes::state::SessionDB* session_db,
                 ContextEngine* context_engine,
                 MemoryManager* memory,
                 PromptBuilder* prompt_builder,
                 ToolDispatcher tool_dispatcher,
                 std::vector<ToolSchema> tool_schemas,
                 AgentCallbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
    if (!llm) throw std::invalid_argument("AIAgent: llm must not be null");
    if (!prompt_builder)
        throw std::invalid_argument("AIAgent: prompt_builder must not be null");
    impl_->config = std::move(config);
    impl_->llm = llm;
    impl_->session_db = session_db;
    impl_->context_engine = context_engine;
    impl_->memory = memory;
    impl_->prompt_builder = prompt_builder;
    impl_->tool_dispatcher = std::move(tool_dispatcher);
    impl_->tool_schemas = std::move(tool_schemas);
    impl_->callbacks = std::move(callbacks);
}

AIAgent::~AIAgent() = default;

void AIAgent::set_memory_store(hermes::state::MemoryStore* store) {
    impl_->memory_store = store;
}

void AIAgent::set_sleep_function(
    std::function<void(std::chrono::milliseconds)> fn) {
    impl_->sleep_fn = std::move(fn);
}

std::string AIAgent::chat(const std::string& message) {
    return run_conversation(message).final_response;
}

ConversationResult AIAgent::run_conversation(
    const std::string& user_message,
    std::optional<std::string> system_message_override,
    std::optional<std::vector<hermes::llm::Message>> conversation_history,
    const std::string& task_id) {
    impl_->stop_requested.store(false);
    std::vector<Message> history;
    if (conversation_history) history = std::move(*conversation_history);
    return impl_->run(user_message, std::move(system_message_override),
                      std::move(history), task_id);
}

void AIAgent::request_stop() { impl_->stop_requested.store(true); }
bool AIAgent::stop_requested() const { return impl_->stop_requested.load(); }

const AgentConfig& AIAgent::config() const { return impl_->config; }
const std::vector<Message>& AIAgent::messages() const { return impl_->messages; }
const CanonicalUsage& AIAgent::total_usage() const {
    return impl_->total_usage;
}

AgentActivitySummary AIAgent::activity_summary() const {
    std::lock_guard<std::mutex> lk(impl_->activity_mutex);
    AgentActivitySummary s;
    s.last_activity_ts = impl_->last_activity_ts;
    s.last_activity_desc = impl_->last_activity_desc;
    double now = Impl::now_seconds();
    s.seconds_since_activity =
        impl_->last_activity_ts > 0.0 ? (now - impl_->last_activity_ts) : 0.0;
    s.current_tool = impl_->current_tool;
    s.api_call_count = impl_->api_call_count;
    s.max_iterations = impl_->config.max_iterations;
    s.budget_used = impl_->api_call_count;
    s.budget_max = impl_->config.max_iterations;
    return s;
}

std::optional<RateLimitState> AIAgent::rate_limit_state() const {
    std::lock_guard<std::mutex> lk(impl_->rl_mutex);
    return impl_->rate_limit_state;
}

void AIAgent::capture_rate_limits(
    const std::unordered_map<std::string, std::string>& headers,
    std::string_view provider) {
    auto state =
        parse_rate_limit_headers(headers, std::string(provider));
    if (state) {
        std::lock_guard<std::mutex> lk(impl_->rl_mutex);
        impl_->rate_limit_state = std::move(state);
    }
}

void AIAgent::invalidate_system_prompt() {
    impl_->system_prompt_invalidated.store(true);
    // Python also reloads memory from disk here so the next prompt
    // build sees any out-of-band writes.  C++ MemoryStore exposes
    // invalidate_cache() which achieves the same effect — subsequent
    // read_all() re-parses the backing files.
    if (impl_->memory_store) {
        try { impl_->memory_store->invalidate_cache(); } catch (...) {}
    }
}

void AIAgent::set_status_callback(StatusCallback cb) {
    impl_->callbacks.on_status = std::move(cb);
}
void AIAgent::set_context_pressure_callback(ContextPressureCallback cb) {
    impl_->callbacks.on_context_pressure = std::move(cb);
}
void AIAgent::set_telemetry_callback(TelemetryCallback cb) {
    impl_->callbacks.on_telemetry = std::move(cb);
}
void AIAgent::set_persist_user_message_override(
    PersistUserMessageOverride cb) {
    impl_->callbacks.persist_user_message_override = std::move(cb);
}

}  // namespace hermes::agent
