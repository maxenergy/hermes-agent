// Delegate + Mixture-of-Agents tools — C++17 port of tools/delegate_tool.py.
//
// See cpp/tools/include/hermes/tools/delegate_tool.hpp for the public
// contract.  This .cpp is the translation of the Python helpers:
//
//     Python                                 → C++
//     ─────────────────────────────────────────────────────────────
//     _get_max_concurrent_children()         get_max_concurrent_children
//     check_delegate_requirements()          check_delegate_requirements
//     _build_child_system_prompt()           build_child_system_prompt
//     _resolve_workspace_hint()              resolve_workspace_hint
//     _strip_blocked_tools()                 strip_blocked_tools
//     _build_child_progress_callback()       build_child_progress_callback
//     _build_child_agent()                   build_child_agent (file-local)
//     _run_single_child()                    run_single_child
//     delegate_task()                        delegate_task
//     _resolve_child_credential_pool()       resolve_child_credential_pool
//     _resolve_delegation_credentials()      resolve_delegation_credentials
//     _load_config()                         DelegateConfig::load / from_json
//
// The policy layer (blocked tools, depth cap, concurrency cap) is honoured
// here; per-child credential isolation is done through the shared
// hermes::llm::CredentialPool; parallel fan-out uses hermes::core::async_bridge.

#include "hermes/tools/delegate_tool.hpp"

#include "hermes/core/async_bridge.hpp"
#include "hermes/llm/credential_pool.hpp"
#include "hermes/tools/registry.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

// ===========================================================================
// Module-level state (process-global — matches Python behaviour).
// ===========================================================================

namespace {

std::mutex           g_opts_mu;
DelegateOptions      g_opts;
bool                 g_registered = false;

// -- helpers ---------------------------------------------------------------

template <typename T>
T from_json_or(const nlohmann::json& j, const std::string& key, T fallback) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    try {
        return j[key].get<T>();
    } catch (...) {
        return fallback;
    }
}

std::string strip(const std::string& s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string lowercased(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

DelegateOptions snapshot_opts() {
    std::lock_guard<std::mutex> lk(g_opts_mu);
    return g_opts;
}

}  // namespace

// ===========================================================================
// Builtin blocked tools — recursion / user-interaction / cross-agent writes.
// ===========================================================================

const std::unordered_set<std::string>& builtin_blocked_tools() {
    static const std::unordered_set<std::string> kBlocked = {
        "delegate_task",   // no recursive delegation
        "clarify",         // no user interaction
        "memory",          // no shared MEMORY.md writes
        "send_message",    // no cross-platform side effects
        "execute_code",    // children should reason step-by-step
    };
    return kBlocked;
}

// ===========================================================================
// DelegateConfig — load + from_json.
// ===========================================================================

DelegateConfig DelegateConfig::from_json(const nlohmann::json& delegation) {
    DelegateConfig c;
    if (!delegation.is_object()) return c;

    if (delegation.contains("max_concurrent_children") &&
        delegation["max_concurrent_children"].is_number_integer()) {
        c.max_concurrent_children =
            delegation["max_concurrent_children"].get<int>();
    }
    if (delegation.contains("max_iterations") &&
        delegation["max_iterations"].is_number_integer()) {
        c.max_iterations = delegation["max_iterations"].get<int>();
    }
    if (delegation.contains("max_depth") &&
        delegation["max_depth"].is_number_integer()) {
        c.max_depth = delegation["max_depth"].get<int>();
    }

    auto take_str = [&](const char* key, std::string& dst) {
        if (delegation.contains(key) && delegation[key].is_string()) {
            dst = delegation[key].get<std::string>();
        }
    };
    take_str("model",            c.model);
    take_str("provider",         c.provider);
    take_str("base_url",         c.base_url);
    take_str("api_key",          c.api_key);
    take_str("api_mode",         c.api_mode);
    take_str("reasoning_effort", c.reasoning_effort);

    if (delegation.contains("blocked_tools") &&
        delegation["blocked_tools"].is_array()) {
        for (const auto& v : delegation["blocked_tools"]) {
            if (v.is_string()) c.extra_blocked_tools.push_back(v.get<std::string>());
        }
    }
    return c;
}

DelegateConfig DelegateConfig::load() {
    // The CLI is expected to call register_delegate_tools() with a
    // DelegateOptions carrying an already-populated DelegateConfig.
    // This fallback returns whatever the registration recorded.
    std::lock_guard<std::mutex> lk(g_opts_mu);
    return g_opts.config;
}

// ===========================================================================
// get_max_concurrent_children — config > env > default.
// ===========================================================================

int get_max_concurrent_children(const DelegateConfig& cfg) {
    if (cfg.max_concurrent_children.has_value()) {
        return std::max(1, *cfg.max_concurrent_children);
    }
    if (const char* env = std::getenv("DELEGATION_MAX_CONCURRENT_CHILDREN")) {
        try {
            return std::max(1, std::stoi(env));
        } catch (...) {
            // fall through
        }
    }
    return kDefaultMaxConcurrentChildren;
}

// ===========================================================================
// check_delegate_requirements — always true (mirrors Python).
// ===========================================================================

bool check_delegate_requirements() { return true; }

// ===========================================================================
// build_child_system_prompt — deterministic prompt assembly.
// ===========================================================================

std::string build_child_system_prompt(const std::string& goal,
                                      const std::string& context,
                                      const std::string& workspace_hint) {
    std::ostringstream os;
    os << "You are a focused subagent working on a specific delegated task.\n"
       << "\n"
       << "YOUR TASK:\n" << goal;

    const auto ctx = strip(context);
    if (!ctx.empty()) {
        os << "\n\nCONTEXT:\n" << ctx;
    }
    const auto ws = strip(workspace_hint);
    if (!ws.empty()) {
        os << "\n\nWORKSPACE PATH:\n" << ws << "\n"
           << "Use this exact path for local repository/workdir operations "
              "unless the task explicitly says otherwise.";
    }
    os << "\n\nComplete this task using the tools available to you. "
       << "When finished, provide a clear, concise summary of:\n"
       << "- What you did\n"
       << "- What you found or accomplished\n"
       << "- Any files you created or modified\n"
       << "- Any issues encountered\n\n"
       << "Important workspace rule: Never assume a repository lives at "
          "/workspace/... or any other container-style path unless the "
          "task/context explicitly gives that path. If no exact local path "
          "is provided, discover it first before issuing git/workdir-specific "
          "commands.\n\n"
       << "Be thorough but concise — your response is returned to the "
          "parent agent as a summary.";
    return os.str();
}

// ===========================================================================
// resolve_workspace_hint — first absolute existing directory, else empty.
// ===========================================================================

std::string resolve_workspace_hint(const ParentContext& parent) {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("TERMINAL_CWD")) {
        candidates.emplace_back(env);
    }
    if (!parent.cwd.empty()) {
        candidates.push_back(parent.cwd);
    }

    for (const auto& raw : candidates) {
        if (raw.empty()) continue;
        std::error_code ec;
        std::filesystem::path p(raw);
        if (!p.is_absolute()) continue;
        auto canon = std::filesystem::weakly_canonical(p, ec);
        if (ec) continue;
        if (std::filesystem::is_directory(canon, ec) && !ec) {
            return canon.string();
        }
    }
    return {};
}

// ===========================================================================
// strip_blocked_tools — builtin blocklist + extras + blocked toolset names.
// ===========================================================================

std::vector<std::string> strip_blocked_tools(
    const std::vector<std::string>& request,
    const std::vector<std::string>& extra_blocked) {
    static const std::unordered_set<std::string> kBlockedToolsets = {
        "delegation", "clarify", "memory", "code_execution",
    };
    std::unordered_set<std::string> extra(extra_blocked.begin(),
                                          extra_blocked.end());
    std::vector<std::string> out;
    out.reserve(request.size());
    for (const auto& ts : request) {
        if (kBlockedToolsets.count(ts)) continue;
        if (extra.count(ts)) continue;
        if (builtin_blocked_tools().count(ts)) continue;  // belt-and-braces
        out.push_back(ts);
    }
    return out;
}

// ===========================================================================
// resolve_child_credential_pool — share parent's pool if providers match.
// ===========================================================================

hermes::llm::CredentialPool* resolve_child_credential_pool(
    const std::string&   effective_provider,
    const ParentContext& parent) {
    if (effective_provider.empty()) {
        return parent.credential_pool;
    }
    if (parent.credential_pool && effective_provider == parent.provider) {
        return parent.credential_pool;
    }
    auto opts = snapshot_opts();
    return opts.credential_pool;
}

// ===========================================================================
// resolve_delegation_credentials — merges config > parent.
// ===========================================================================

ChildCredentials resolve_delegation_credentials(const DelegateConfig& cfg,
                                                const ParentContext&  parent) {
    ChildCredentials out;
    out.model = strip(cfg.model);

    const auto base = strip(cfg.base_url);
    if (!base.empty()) {
        std::string api_key = strip(cfg.api_key);
        if (api_key.empty()) {
            if (const char* env = std::getenv("OPENAI_API_KEY")) {
                api_key = strip(env);
            }
        }
        if (api_key.empty()) {
            throw std::invalid_argument(
                "Delegation base_url is configured but no API key was found. "
                "Set delegation.api_key or OPENAI_API_KEY.");
        }

        const auto base_lower = lowercased(base);
        std::string provider = "custom";
        std::string api_mode = "chat_completions";
        if (base_lower.find("chatgpt.com/backend-api/codex") != std::string::npos) {
            provider = "openai-codex";
            api_mode = "codex_responses";
        } else if (base_lower.find("api.anthropic.com") != std::string::npos) {
            provider = "anthropic";
            api_mode = "anthropic_messages";
        }

        out.provider = provider;
        out.base_url = base;
        out.api_key  = api_key;
        out.api_mode = api_mode;
        return out;
    }

    const auto prov = strip(cfg.provider);
    if (prov.empty()) {
        // No override — child inherits from parent at runtime.
        return out;
    }

    // Provider configured without base_url.  The Python path calls into
    // runtime_provider; here we require an explicit api_key alongside.
    const auto key = strip(cfg.api_key);
    if (key.empty()) {
        throw std::invalid_argument(
            std::string("Delegation provider '") + prov +
            "' requires an api_key/base_url pair in config.yaml "
            "(C++ build has no runtime_provider resolver yet).");
    }
    out.provider = prov;
    out.api_key  = key;
    out.api_mode = strip(cfg.api_mode);
    out.base_url = parent.base_url;
    return out;
}

// ===========================================================================
// build_child_progress_callback — forwards child events to parent UI.
// ===========================================================================

ProgressCallback build_child_progress_callback(int task_index,
                                               int task_count,
                                               const ParentContext& parent) {
    if (!parent.progress_callback) {
        return nullptr;
    }
    auto parent_cb = parent.progress_callback;
    return [parent_cb, task_index, task_count](const ProgressEvent& ev) {
        ProgressEvent copy = ev;
        copy.task_index = task_index;
        copy.task_count = task_count;
        // "tool.completed" is a no-op for gateway display (Python parity:
        // the spinner only draws on .started).
        if (copy.event_type == "tool.completed") return;
        try {
            parent_cb(copy);
        } catch (...) {
            // swallow — progress failures must not abort delegation
        }
    };
}

// ===========================================================================
// Default AIAgent virtuals — legacy `run()` impls continue to work via the
// fallback here (run_with_context calls run()).
// ===========================================================================

ChildResult AIAgent::run_with_context(const std::string& goal,
                                      const std::string& /*system_prompt*/,
                                      const std::vector<std::string>& /*toolsets*/,
                                      const ChildCredentials& creds,
                                      ProgressCallback cb) {
    stashed_cb_ = std::move(cb);
    ChildResult r;
    r.model = creds.model;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        std::string constraints;
        std::string output = run(std::string("Goal: ") + goal, constraints);
        r.summary     = output;
        r.status      = "completed";
        r.exit_reason = "completed";
    } catch (const std::exception& ex) {
        r.status = "error";
        r.error  = ex.what();
    } catch (...) {
        r.status = "error";
        r.error  = "unknown exception in child agent";
    }
    const auto t1 = std::chrono::steady_clock::now();
    r.duration_seconds =
        std::chrono::duration<double>(t1 - t0).count();
    return r;
}

void AIAgent::on_child_tool_event(const ProgressEvent& ev) {
    if (stashed_cb_) {
        try {
            stashed_cb_(ev);
        } catch (...) {
            // ignore
        }
    }
}

// ===========================================================================
// ChildResult::to_json.
// ===========================================================================

nlohmann::json ChildResult::to_json() const {
    nlohmann::json j;
    j["task_index"]       = task_index;
    j["status"]           = status;
    j["summary"]          = summary;
    j["api_calls"]        = api_calls;
    j["duration_seconds"] = duration_seconds;
    j["exit_reason"]      = exit_reason;
    j["model"]            = model;
    if (!error.empty()) j["error"] = error;
    return j;
}

// ===========================================================================
// parse_tasks_array.
// ===========================================================================

std::vector<TaskSpec> parse_tasks_array(const nlohmann::json& tasks,
                                        std::string&          error) {
    std::vector<TaskSpec> out;
    if (!tasks.is_array()) {
        error = "'tasks' must be an array";
        return out;
    }
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& t = tasks[i];
        if (!t.is_object()) {
            error = "tasks[" + std::to_string(i) + "] is not an object";
            return {};
        }
        TaskSpec spec;
        spec.goal    = strip(from_json_or<std::string>(t, "goal",    ""));
        spec.context = from_json_or<std::string>(t, "context", "");
        if (t.contains("toolsets") && t["toolsets"].is_array()) {
            for (const auto& ts : t["toolsets"]) {
                if (ts.is_string()) spec.toolsets.push_back(ts.get<std::string>());
            }
        }
        if (spec.goal.empty()) {
            error = "tasks[" + std::to_string(i) + "] is missing a 'goal'";
            return {};
        }
        out.push_back(std::move(spec));
    }
    return out;
}

// ===========================================================================
// build_child_agent — resolves toolsets, creds, prompt; returns nullptr-
// agent when no factory is wired.
// ===========================================================================

namespace {

struct BuiltChild {
    std::unique_ptr<AIAgent>  agent;
    std::string               system_prompt;
    std::vector<std::string>  toolsets;
    ChildCredentials          creds;
    ProgressCallback          cb;
};

BuiltChild build_child_agent(int task_index,
                             int task_count,
                             const TaskSpec&        task,
                             const ChildCredentials& override_creds,
                             int                    max_iterations,
                             const ParentContext&   parent,
                             const DelegateOptions& opts) {
    BuiltChild built;

    // Effective toolsets: intersect requested with parent; filter blocked.
    std::vector<std::string> requested = task.toolsets;
    if (requested.empty()) {
        requested = parent.enabled_toolsets;
    }
    built.toolsets = strip_blocked_tools(requested, opts.config.extra_blocked_tools);

    // Effective creds (override > inherit).
    ChildCredentials eff = override_creds;
    if (eff.model.empty())    eff.model    = parent.model;
    if (eff.provider.empty()) eff.provider = parent.provider;
    if (eff.base_url.empty()) eff.base_url = parent.base_url;
    if (eff.api_key.empty())  eff.api_key  = parent.api_key;
    if (eff.api_mode.empty()) eff.api_mode = parent.api_mode;
    built.creds = eff;

    // System prompt.
    built.system_prompt = build_child_system_prompt(
        task.goal, task.context, resolve_workspace_hint(parent));

    // Progress callback.
    built.cb = build_child_progress_callback(task_index, task_count, parent);

    // Factory (rich preferred, fallback to legacy).
    if (opts.rich_factory) {
        built.agent = opts.rich_factory(eff, parent, task_index);
    } else if (opts.legacy_factory) {
        built.agent = opts.legacy_factory(eff.model);
    }
    if (built.agent) {
        built.agent->set_max_iterations(max_iterations);
    }
    return built;
}

}  // namespace

// ===========================================================================
// run_single_child — orchestrates one delegation.
// ===========================================================================

ChildResult run_single_child(int task_index,
                             const std::string&     goal,
                             const std::string&     context,
                             const std::vector<std::string>& toolsets,
                             const ChildCredentials& creds,
                             const ParentContext&   parent,
                             const DelegateOptions& opts) {
    const auto t0 = std::chrono::steady_clock::now();

    // Save/restore parent's resolved tool names (process-global).
    auto saved_tool_names = ToolRegistry::instance().last_resolved_tool_names();

    // Lease a credential from the pool before construction.
    hermes::llm::CredentialPool* pool =
        resolve_child_credential_pool(creds.provider, parent);
    ChildCredentials eff = creds;
    if (pool) {
        if (auto leased = pool->get(creds.provider.empty() ? parent.provider
                                                           : creds.provider)) {
            if (eff.api_key.empty())  eff.api_key  = leased->api_key;
            if (eff.base_url.empty()) eff.base_url = leased->base_url;
        }
    }

    TaskSpec spec;
    spec.goal     = goal;
    spec.context  = context;
    spec.toolsets = toolsets;

    auto built = build_child_agent(
        task_index, /*task_count*/ 1, spec, eff,
        opts.config.max_iterations.value_or(kDefaultMaxIterations),
        parent, opts);

    ChildResult r;
    r.task_index = task_index;
    r.model      = built.creds.model;
    if (!built.agent) {
        r.status = "error";
        r.error  = "delegate requires agent factory — "
                   "call register_delegate_tools() with a factory at startup";
        r.duration_seconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
        ToolRegistry::instance().set_last_resolved_tool_names(saved_tool_names);
        return r;
    }

    try {
        auto cr = built.agent->run_with_context(
            goal, built.system_prompt, built.toolsets, built.creds, built.cb);
        cr.task_index = task_index;
        if (cr.model.empty()) cr.model = built.creds.model;
        if (cr.status.empty()) cr.status = "completed";
        if (cr.exit_reason.empty()) cr.exit_reason = "completed";
        r = cr;
    } catch (const std::exception& ex) {
        r.status = "error";
        r.error  = ex.what();
    } catch (...) {
        r.status = "error";
        r.error  = "unknown exception";
    }

    r.duration_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

    ToolRegistry::instance().set_last_resolved_tool_names(saved_tool_names);
    return r;
}

// ===========================================================================
// delegate_task — public dispatcher.
// ===========================================================================

std::string delegate_task(const nlohmann::json& args,
                          const ParentContext&  parent,
                          const DelegateOptions& opts) {
    // Depth check first — cheapest rejection path.
    const int max_depth = opts.config.max_depth.value_or(kDefaultMaxDepth);
    if (parent.depth >= max_depth) {
        return tool_error(
            std::string("Delegation depth limit reached (") +
            std::to_string(max_depth) +
            "). Subagents cannot spawn further subagents.");
    }

    if (!opts.rich_factory && !opts.legacy_factory) {
        return tool_error(
            "delegate requires agent factory — call register_delegate_tools() "
            "with a factory at startup");
    }

    // Resolve credentials from config + parent.
    ChildCredentials creds;
    try {
        creds = resolve_delegation_credentials(opts.config, parent);
    } catch (const std::exception& ex) {
        return tool_error(ex.what());
    }

    // Normalize into task list.
    std::vector<TaskSpec> task_list;
    const int max_children = get_max_concurrent_children(opts.config);

    if (args.contains("tasks") && args["tasks"].is_array() &&
        !args["tasks"].empty()) {
        std::string parse_err;
        task_list = parse_tasks_array(args["tasks"], parse_err);
        if (!parse_err.empty()) {
            return tool_error(parse_err);
        }
        if (static_cast<int>(task_list.size()) > max_children) {
            return tool_error(
                std::string("Too many tasks: ") +
                std::to_string(task_list.size()) +
                " provided, but max_concurrent_children is " +
                std::to_string(max_children) + ".");
        }
    } else {
        const auto goal = strip(from_json_or<std::string>(args, "goal", ""));
        if (goal.empty()) {
            return tool_error("Provide either 'goal' (single task) "
                              "or 'tasks' (batch).");
        }
        TaskSpec t;
        t.goal    = goal;
        t.context = from_json_or<std::string>(args, "context", "");
        if (args.contains("toolsets") && args["toolsets"].is_array()) {
            for (const auto& v : args["toolsets"]) {
                if (v.is_string()) t.toolsets.push_back(v.get<std::string>());
            }
        }
        task_list.push_back(std::move(t));
    }

    // Fold top-level toolsets into each task entry if empty.
    if (args.contains("toolsets") && args["toolsets"].is_array()) {
        std::vector<std::string> fallback;
        for (const auto& v : args["toolsets"]) {
            if (v.is_string()) fallback.push_back(v.get<std::string>());
        }
        for (auto& t : task_list) {
            if (t.toolsets.empty()) t.toolsets = fallback;
        }
    }

    // Optional caller override of max_iterations.
    int effective_max_iter = opts.config.max_iterations.value_or(
        kDefaultMaxIterations);
    if (args.contains("max_iterations") &&
        args["max_iterations"].is_number_integer()) {
        effective_max_iter = std::max(1, args["max_iterations"].get<int>());
    }

    DelegateOptions eff_opts = opts;
    eff_opts.config.max_iterations = effective_max_iter;

    // Fan out.
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ChildResult> results;
    results.reserve(task_list.size());

    if (task_list.size() == 1) {
        results.push_back(run_single_child(
            0, task_list[0].goal, task_list[0].context, task_list[0].toolsets,
            creds, parent, eff_opts));
    } else {
        std::vector<std::future<ChildResult>> futs;
        futs.reserve(task_list.size());
        for (std::size_t i = 0; i < task_list.size(); ++i) {
            const auto& t = task_list[i];
            futs.push_back(hermes::core::run_async(
                [i, t, creds, parent, eff_opts]() -> ChildResult {
                    return run_single_child(
                        static_cast<int>(i), t.goal, t.context, t.toolsets,
                        creds, parent, eff_opts);
                }));
        }
        auto settled = hermes::core::join_all_settled(std::move(futs));
        for (std::size_t i = 0; i < settled.size(); ++i) {
            if (settled[i].ok) {
                results.push_back(std::move(settled[i].value));
            } else {
                ChildResult r;
                r.task_index = static_cast<int>(i);
                r.status     = "error";
                r.error      = settled[i].error;
                results.push_back(std::move(r));
            }
        }
        std::sort(results.begin(), results.end(),
                  [](const ChildResult& a, const ChildResult& b) {
                      return a.task_index < b.task_index;
                  });
    }

    const double total =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : results) arr.push_back(r.to_json());
    nlohmann::json env = {
        {"results",                arr},
        {"total_duration_seconds", total},
    };
    return tool_result(env);
}

// ===========================================================================
// mixture_of_agents handler — simple parallel-call behaviour.
// ===========================================================================

namespace {

std::string handle_mixture_of_agents(const nlohmann::json& args,
                                     const ToolContext& /*ctx*/) {
    auto opts = snapshot_opts();
    if (!opts.rich_factory && !opts.legacy_factory) {
        return tool_error("mixture_of_agents requires agent factory — "
                          "call register_delegate_tools() at startup");
    }

    const auto prompt = args.at("prompt").get<std::string>();
    const auto& models_arr = args.at("models");
    if (!models_arr.is_array() || models_arr.empty()) {
        return tool_error("models must be a non-empty array of model names");
    }
    std::vector<std::string> models;
    for (const auto& m : models_arr) {
        if (m.is_string()) models.push_back(m.get<std::string>());
    }

    auto saved = ToolRegistry::instance().last_resolved_tool_names();

    std::vector<std::future<std::string>> futs;
    futs.reserve(models.size());
    for (const auto& model : models) {
        futs.push_back(hermes::core::run_async(
            [prompt, model, opts]() -> std::string {
                std::unique_ptr<AIAgent> agent;
                if (opts.rich_factory) {
                    ChildCredentials c;
                    c.model = model;
                    ParentContext p;
                    agent = opts.rich_factory(c, p, 0);
                } else {
                    agent = opts.legacy_factory(model);
                }
                if (!agent) {
                    return "(error: factory returned nullptr for " + model + ")";
                }
                return agent->run(prompt, "");
            }));
    }

    nlohmann::json responses = nlohmann::json::array();
    for (std::size_t i = 0; i < futs.size(); ++i) {
        nlohmann::json entry;
        entry["model"] = models[i];
        try {
            entry["response"] = futs[i].get();
        } catch (const std::exception& ex) {
            entry["response"] = std::string("(error: ") + ex.what() + ")";
        }
        responses.push_back(std::move(entry));
    }

    ToolRegistry::instance().set_last_resolved_tool_names(saved);

    return tool_result({
        {"responses",   responses},
        {"model_count", static_cast<int>(models.size())},
    });
}

// ---------------------------------------------------------------------------
// Tool registration entrypoints.
// ---------------------------------------------------------------------------

std::string handle_delegate_task(const nlohmann::json& args,
                                 const ToolContext&    /*ctx*/) {
    auto opts = snapshot_opts();
    ParentContext parent;
    if (opts.parent_accessor) {
        try { parent = opts.parent_accessor(); } catch (...) {}
    }
    return delegate_task(args, parent, opts);
}

void register_schemas() {
    auto& reg = ToolRegistry::instance();
    {
        ToolEntry e;
        e.name        = "delegate_task";
        e.toolset     = "moa";
        e.description = "Delegate a task (or batch) to subagents in isolated contexts";
        e.emoji       = "\xF0\x9F\x94\x80";  // shuffle
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"goal",        {{"type", "string"},  {"description", "Single-task goal"}}},
                {"context",     {{"type", "string"},  {"description", "Background for the subagent"}}},
                {"constraints", {{"type", "string"},  {"description", "Optional constraints"}}},
                {"model",       {{"type", "string"},  {"description", "Optional per-call model"}}},
                {"toolsets",    {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"tasks",       {{"type", "array"}, {"description", "Batch mode — array of {goal, context, toolsets}"}}},
                {"max_iterations", {{"type", "integer"}}},
            }},
            // `goal` OR `tasks` — handler validates at runtime.
            {"required", nlohmann::json::array()},
        };
        e.handler = handle_delegate_task;
        reg.register_tool(std::move(e));
    }
    {
        ToolEntry e;
        e.name        = "mixture_of_agents";
        e.toolset     = "moa";
        e.description = "Call multiple LLMs in parallel and aggregate";
        e.emoji       = "\xF0\x9F\x94\x80";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"}, {"description", "Prompt to send"}}},
                {"models", {{"type", "array"},  {"items", {{"type", "string"}}},
                            {"description", "Model names to poll"}}},
            }},
            {"required", nlohmann::json::array({"prompt", "models"})},
        };
        e.handler = handle_mixture_of_agents;
        reg.register_tool(std::move(e));
    }
}

}  // namespace

void register_delegate_tools(AgentFactory factory) {
    DelegateOptions opts;
    opts.legacy_factory = std::move(factory);
    register_delegate_tools(std::move(opts));
}

void register_delegate_tools(DelegateOptions opts) {
    {
        std::lock_guard<std::mutex> lk(g_opts_mu);
        g_opts        = std::move(opts);
        g_registered = true;
    }
    register_schemas();
}

void unregister_delegate_tools() {
    {
        std::lock_guard<std::mutex> lk(g_opts_mu);
        g_opts        = DelegateOptions{};
        g_registered = false;
    }
    auto& reg = ToolRegistry::instance();
    reg.deregister("delegate_task");
    reg.deregister("mixture_of_agents");
}

}  // namespace hermes::tools
