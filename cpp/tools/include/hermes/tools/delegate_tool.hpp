// Delegate + Mixture-of-Agents tools — C++17 port of tools/delegate_tool.py.
//
// The Python original (tools/delegate_tool.py, ~1088 LoC) spawns child
// AIAgent instances with isolated context, restricted toolsets, their own
// terminal session, and per-provider credential pools.  The parent blocks
// until every child finishes and only the summary is returned.
//
// The C++ port keeps the same behavioural surface, trimmed to what the
// static-C++ agent loop needs:
//
//   * Policy layer (blocked tools, depth cap, concurrency cap)
//   * Per-child credential isolation via hermes::llm::CredentialPool
//   * Progress callback forwarding (parent observes nested tool events)
//   * Parallel child fan-out via hermes::core::async_bridge
//   * mixture_of_agents parallel invocation
//
// The CLI wires a CliSubAgent factory; that contract (AgentFactory taking a
// model string and returning a unique_ptr<AIAgent>) must keep working.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::llm {
class CredentialPool;
struct PooledCredential;
}  // namespace hermes::llm

namespace hermes::tools {

// ---------------------------------------------------------------------------
// Progress callback — mirrors the Python `tool_progress_callback` shape.
//   event_type ∈ {"tool.started", "tool.completed", "reasoning.available",
//                  "_thinking", "subagent_progress"}
// ---------------------------------------------------------------------------
struct ProgressEvent {
    std::string event_type;
    std::string tool_name;
    std::string preview;
    int         task_index = 0;
    int         task_count = 1;
};

using ProgressCallback = std::function<void(const ProgressEvent&)>;

// ---------------------------------------------------------------------------
// Child credential bundle — any field empty = "inherit from parent".
// ---------------------------------------------------------------------------
struct ChildCredentials {
    std::string provider;
    std::string base_url;
    std::string api_key;
    std::string api_mode;
    std::string model;
};

// ---------------------------------------------------------------------------
// Structured per-task result returned from run_single_child().
// ---------------------------------------------------------------------------
struct ChildResult {
    int         task_index      = 0;
    std::string status;             // "completed" | "failed" | "interrupted" | "error"
    std::string summary;
    std::string error;
    int         api_calls        = 0;
    double      duration_seconds = 0.0;
    std::string exit_reason;        // "completed" | "max_iterations" | "interrupted"
    std::string model;

    nlohmann::json to_json() const;
};

// ---------------------------------------------------------------------------
// Parent context — subset of AIAgent state the delegate layer reads.
// ---------------------------------------------------------------------------
struct ParentContext {
    std::string                provider;
    std::string                base_url;
    std::string                api_key;
    std::string                api_mode;
    std::string                model;
    std::string                cwd;
    int                        depth = 0;
    std::vector<std::string>   enabled_toolsets;
    hermes::llm::CredentialPool* credential_pool = nullptr;
    ProgressCallback           progress_callback;
};

// ---------------------------------------------------------------------------
// AIAgent — subagent interface.  `run()` is the legacy CLI entrypoint;
// `run_with_context()` carries the richer child setup for production use.
// ---------------------------------------------------------------------------
class AIAgent {
public:
    virtual ~AIAgent() = default;

    virtual std::string run(const std::string& goal,
                            const std::string& constraints) = 0;

    virtual ChildResult run_with_context(const std::string&       goal,
                                         const std::string&       system_prompt,
                                         const std::vector<std::string>& toolsets,
                                         const ChildCredentials&  creds,
                                         ProgressCallback         cb);

    virtual void on_child_tool_event(const ProgressEvent& ev);
    virtual void set_max_iterations(int n) { max_iterations_ = n; }
    virtual std::string session_id() const { return {}; }

protected:
    int              max_iterations_ = 50;
    ProgressCallback stashed_cb_;
};

using AgentFactory = std::function<std::unique_ptr<AIAgent>(const std::string& model)>;

using RichAgentFactory = std::function<std::unique_ptr<AIAgent>(
    const ChildCredentials& creds,
    const ParentContext&    parent,
    int                     task_index)>;

// ---------------------------------------------------------------------------
// Delegate config — mirrors the `delegation:` block of config.yaml.
// ---------------------------------------------------------------------------
struct DelegateConfig {
    std::optional<int>         max_concurrent_children;
    std::optional<int>         max_iterations;
    std::optional<int>         max_depth;
    std::string                model;
    std::string                provider;
    std::string                base_url;
    std::string                api_key;
    std::string                api_mode;
    std::string                reasoning_effort;
    std::vector<std::string>   extra_blocked_tools;

    static DelegateConfig from_json(const nlohmann::json& delegation);
    static DelegateConfig load();
};

// ---------------------------------------------------------------------------
// Runtime options — set once by the CLI / gateway at registration time.
// ---------------------------------------------------------------------------
struct DelegateOptions {
    DelegateConfig              config;
    AgentFactory                legacy_factory;
    RichAgentFactory            rich_factory;
    hermes::llm::CredentialPool* credential_pool = nullptr;
    std::function<ParentContext()> parent_accessor;
};

// ---------------------------------------------------------------------------
// Public registration entrypoints.
// ---------------------------------------------------------------------------
void register_delegate_tools(AgentFactory factory = nullptr);
void register_delegate_tools(DelegateOptions opts);
void unregister_delegate_tools();

// ---------------------------------------------------------------------------
// Exposed helpers — mirror the Python `_*` helpers.
// ---------------------------------------------------------------------------
const std::unordered_set<std::string>& builtin_blocked_tools();

constexpr int kDefaultMaxConcurrentChildren = 3;
constexpr int kDefaultMaxIterations         = 50;
constexpr int kDefaultMaxDepth              = 2;

int get_max_concurrent_children(const DelegateConfig& cfg);

bool check_delegate_requirements();

std::string build_child_system_prompt(const std::string& goal,
                                      const std::string& context,
                                      const std::string& workspace_hint);

std::string resolve_workspace_hint(const ParentContext& parent);

std::vector<std::string> strip_blocked_tools(
    const std::vector<std::string>& request,
    const std::vector<std::string>& extra_blocked = {});

hermes::llm::CredentialPool* resolve_child_credential_pool(
    const std::string& effective_provider,
    const ParentContext& parent);

ChildCredentials resolve_delegation_credentials(const DelegateConfig& cfg,
                                                const ParentContext&  parent);

ProgressCallback build_child_progress_callback(int task_index,
                                               int task_count,
                                               const ParentContext& parent);

ChildResult run_single_child(int task_index,
                             const std::string&     goal,
                             const std::string&     context,
                             const std::vector<std::string>& toolsets,
                             const ChildCredentials& creds,
                             const ParentContext&   parent,
                             const DelegateOptions& opts);

struct TaskSpec {
    std::string              goal;
    std::string              context;
    std::vector<std::string> toolsets;
};
std::vector<TaskSpec> parse_tasks_array(const nlohmann::json& tasks,
                                        std::string&          error);

std::string delegate_task(const nlohmann::json& args,
                          const ParentContext&  parent,
                          const DelegateOptions& opts);

}  // namespace hermes::tools
