#include "hermes/agent/ai_agent.hpp"

#include "hermes/core/path.hpp"
#include "hermes/llm/error_classifier.hpp"
#include "hermes/llm/model_metadata.hpp"
#include "hermes/llm/prompt_cache.hpp"
#include "hermes/llm/retry_policy.hpp"
#include "hermes/state/trajectory.hpp"

#include <atomic>
#include <chrono>
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
            persist_message(u, static_cast<int>(messages.size()));
            messages.push_back(std::move(u));
        }

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
                messages = context_engine->compress(
                    std::move(messages), cur, config.max_context_tokens);
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
                        messages = context_engine->compress(
                            std::move(messages), budget_tokens * 2,
                            budget_tokens);
                        req.messages = messages;
                        retried_compression = true;
                        continue;
                    }
                    if (cls.reason == FailoverReason::RateLimit) {
                        do_sleep(hermes::llm::backoff_for_error(attempt, cls));
                        continue;
                    }
                    if (cls.reason == FailoverReason::ServerError ||
                        cls.reason == FailoverReason::Timeout ||
                        cls.reason == FailoverReason::NetworkError) {
                        do_sleep(hermes::llm::backoff_for_error(attempt, cls));
                        continue;
                    }
                    // Auth / unknown / model unavailable: fatal.
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
                save_trajectory(result);
                return result;
            }

            // 7. Dispatch tool calls.
            for (const auto& tc : resp.assistant_message.tool_calls) {
                if (stop_requested.load()) break;
                emit_tool_call(tc.name, tc.arguments);
                std::string tool_result = dispatch_tool(tc, task_id);
                Message tr = make_tool_result_message(tc.id, tool_result);
                emit_tool_result(tc.name, tool_result);
                persist_message(tr, static_cast<int>(messages.size()));
                messages.push_back(std::move(tr));
            }

            ++api_calls;
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

}  // namespace hermes::agent
