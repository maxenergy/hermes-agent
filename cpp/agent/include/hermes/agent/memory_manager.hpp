// MemoryManager — orchestrates the built-in memory provider plus at most
// ONE external plugin memory provider.
//
// C++17 port of agent/memory_manager.py. Single integration point for the
// run_agent loop — fans out prefetch / sync / lifecycle callbacks to
// every registered provider. Failures in one provider never block the
// others.
//
// The BuiltinMemoryProvider is always registered first and cannot be
// removed. At most ONE external (non-builtin) provider is allowed at a
// time — a second attempt throws std::invalid_argument.
#pragma once

#include "hermes/agent/memory_provider.hpp"
#include "hermes/llm/message.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hermes::agent {

// Strip any "<memory-context>" / "</memory-context>" tags from provider
// output so a malicious recall cannot terminate the fence.
std::string sanitize_memory_context(const std::string& text);

// Wrap `raw_context` in a fenced memory-context block with the system
// note. Returns empty string when `raw_context` is blank.
std::string build_memory_context_block(const std::string& raw_context);

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // Registration / removal ---------------------------------------------

    // Throws std::invalid_argument if the new provider would violate
    // the 1-builtin-1-external rule, or if a provider with the same
    // name is already registered.
    void add_provider(std::unique_ptr<MemoryProvider> p);
    void remove_provider(std::string_view name);

    // System prompt ------------------------------------------------------

    // Concatenate every provider's section, separated by blank lines.
    std::string build_system_prompt();

    // Prefetch -----------------------------------------------------------

    // Fire prefetch on every provider; the legacy side-effect API.
    void prefetch_all(std::string_view user_message);

    // Collect prefetch_string() output from every provider and return
    // the fenced block ready to append to the next user message. Returns
    // empty string when no provider produced any context.
    std::string prefetch_all_string(const std::string& user_message,
                                    const std::string& session_id = "");

    // Queue the next-turn prefetches on every provider.
    void queue_prefetch_all(const std::string& user_message,
                            const std::string& session_id = "");

    // Sync ---------------------------------------------------------------

    // Legacy fire-and-forget sync (calls sync() on each provider).
    void sync_all(std::string_view user_msg,
                  std::string_view assistant_response);

    // Modern sync_turn (carries session_id).
    void sync_turn_all(const std::string& user_msg,
                       const std::string& assistant_response,
                       const std::string& session_id = "");

    // Tool dispatch ------------------------------------------------------

    // Rebuild the tool→provider index. Call after add/remove_provider if
    // the calling agent wants to dispatch tool calls through the manager.
    void rebuild_tool_index();

    // Aggregate every provider's tool schemas into a single list.
    std::vector<nlohmann::json> get_all_tool_schemas();

    // Dispatch a tool call to the owning provider. Returns the JSON
    // result string, or an error object if no provider claims the tool.
    std::string handle_tool_call(const std::string& tool_name,
                                 const nlohmann::json& args,
                                 const MemoryProviderContext& ctx);

    // Lifecycle hooks ----------------------------------------------------

    void initialize_all(const MemoryProviderContext& ctx);
    void shutdown_all();
    void on_turn_start_all(int turn_number,
                           const std::string& user_message,
                           const MemoryProviderContext& ctx);
    void on_session_end_all(
        const std::vector<hermes::llm::Message>& messages,
        const MemoryProviderContext& ctx);
    std::string on_pre_compress_all(
        const std::vector<hermes::llm::Message>& messages);
    void on_delegation_all(const std::string& task,
                           const std::string& result,
                           const std::string& child_session_id,
                           const MemoryProviderContext& ctx);
    void on_memory_write_all(const std::string& action,
                             const std::string& target,
                             const std::string& content);

    // Inspection ---------------------------------------------------------

    std::size_t provider_count() const;
    bool has_external_provider() const;
    std::vector<std::string> provider_names() const;

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<MemoryProvider>> providers_;
    std::unordered_map<std::string, MemoryProvider*> tool_to_provider_;
    std::thread prefetch_thread_;
};

}  // namespace hermes::agent
