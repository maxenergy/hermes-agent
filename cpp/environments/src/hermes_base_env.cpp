// See hermes_base_env.hpp.
#include "hermes/environments/hermes_base_env.hpp"

#include <cstdlib>
#include <sstream>
#include <utility>

namespace hermes::environments::base_env {

namespace {

std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...";
}

std::string trunc_no_ellipsis(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n);
}

// Set an env var (POSIX setenv) and return the old value if any.
void set_env(const std::string& key, const std::string& value) {
    ::setenv(key.c_str(), value.c_str(), /*overwrite=*/1);
}

}  // namespace

// ============================================================================
// BudgetConfig
// ============================================================================
nlohmann::json BudgetConfig::to_json() const {
    nlohmann::json j;
    j["default_result_size"] = default_result_size;
    j["turn_budget"]         = turn_budget;
    j["preview_size"]        = preview_size;
    j["tool_overrides"]      = tool_overrides;
    return j;
}

// ============================================================================
// HermesAgentEnvConfig
// ============================================================================
BudgetConfig HermesAgentEnvConfig::build_budget_config() const {
    BudgetConfig cfg;
    cfg.default_result_size = default_result_size_chars;
    cfg.turn_budget         = turn_budget_chars;
    cfg.preview_size        = preview_size_chars;
    if (tool_result_overrides.has_value()) {
        cfg.tool_overrides = *tool_result_overrides;
    }
    return cfg;
}

std::vector<ConfigError> HermesAgentEnvConfig::validate() const {
    std::vector<ConfigError> errs;
    if (max_agent_turns < 1)
        errs.push_back({"max_agent_turns", "must be >= 1"});
    if (agent_temperature < 0.0)
        errs.push_back({"agent_temperature", "must be >= 0"});
    if (terminal_timeout < 1)
        errs.push_back({"terminal_timeout", "must be >= 1"});
    if (terminal_lifetime < terminal_timeout)
        errs.push_back({"terminal_lifetime", "must be >= terminal_timeout"});
    if (tool_pool_size < 1)
        errs.push_back({"tool_pool_size", "must be >= 1"});
    if (default_result_size_chars < 0)
        errs.push_back({"default_result_size_chars", "must be >= 0"});
    if (turn_budget_chars < 0)
        errs.push_back({"turn_budget_chars", "must be >= 0"});
    if (preview_size_chars < 0)
        errs.push_back({"preview_size_chars", "must be >= 0"});
    if (enabled_toolsets.has_value() && distribution.has_value()) {
        errs.push_back({"distribution", "mutually exclusive with enabled_toolsets"});
    }
    static const std::unordered_set<std::string> kBackends = {
        "local", "docker", "modal", "daytona", "ssh", "singularity",
    };
    if (kBackends.find(terminal_backend) == kBackends.end()) {
        errs.push_back({"terminal_backend", "must be one of local|docker|modal|daytona|ssh|singularity"});
    }
    return errs;
}

namespace {

template <typename T>
void load(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key) || j[key].is_null()) return;
    out = j[key].get<T>();
}

template <typename T>
void load_optional(const nlohmann::json& j, const char* key, std::optional<T>& out) {
    if (!j.contains(key) || j[key].is_null()) return;
    out = j[key].get<T>();
}

}  // namespace

HermesAgentEnvConfig HermesAgentEnvConfig::from_json(const nlohmann::json& j) {
    HermesAgentEnvConfig c;
    if (!j.is_object()) return c;
    load_optional(j, "enabled_toolsets",  c.enabled_toolsets);
    load_optional(j, "disabled_toolsets", c.disabled_toolsets);
    load_optional(j, "distribution",      c.distribution);
    load(j, "max_agent_turns",   c.max_agent_turns);
    load_optional(j, "system_prompt", c.system_prompt);
    load(j, "agent_temperature", c.agent_temperature);
    load(j, "terminal_backend",  c.terminal_backend);
    load(j, "terminal_timeout",  c.terminal_timeout);
    load(j, "terminal_lifetime", c.terminal_lifetime);
    load_optional(j, "dataset_name", c.dataset_name);
    load(j, "dataset_split",    c.dataset_split);
    load(j, "prompt_field",     c.prompt_field);
    load(j, "tool_pool_size",   c.tool_pool_size);
    load(j, "tool_call_parser", c.tool_call_parser);
    load(j, "default_result_size_chars", c.default_result_size_chars);
    load(j, "turn_budget_chars",         c.turn_budget_chars);
    load(j, "preview_size_chars",        c.preview_size_chars);
    if (j.contains("tool_result_overrides") && j["tool_result_overrides"].is_object()) {
        std::unordered_map<std::string, int> m;
        for (auto it = j["tool_result_overrides"].begin();
             it != j["tool_result_overrides"].end(); ++it) {
            if (it.value().is_number_integer()) m[it.key()] = it.value().get<int>();
        }
        c.tool_result_overrides = std::move(m);
    }
    if (j.contains("extra_body") && !j["extra_body"].is_null()) {
        c.extra_body = j["extra_body"];
    }
    return c;
}

nlohmann::json HermesAgentEnvConfig::to_json() const {
    nlohmann::json j;
    if (enabled_toolsets.has_value())  j["enabled_toolsets"]  = *enabled_toolsets;
    else j["enabled_toolsets"] = nullptr;
    if (disabled_toolsets.has_value()) j["disabled_toolsets"] = *disabled_toolsets;
    else j["disabled_toolsets"] = nullptr;
    if (distribution.has_value())      j["distribution"]      = *distribution;
    else j["distribution"] = nullptr;

    j["max_agent_turns"]   = max_agent_turns;
    j["system_prompt"]     = system_prompt.has_value()
        ? nlohmann::json(*system_prompt) : nlohmann::json(nullptr);
    j["agent_temperature"] = agent_temperature;

    j["terminal_backend"]  = terminal_backend;
    j["terminal_timeout"]  = terminal_timeout;
    j["terminal_lifetime"] = terminal_lifetime;

    j["dataset_name"]  = dataset_name.has_value() ? nlohmann::json(*dataset_name) : nlohmann::json(nullptr);
    j["dataset_split"] = dataset_split;
    j["prompt_field"]  = prompt_field;

    j["tool_pool_size"]   = tool_pool_size;
    j["tool_call_parser"] = tool_call_parser;

    j["default_result_size_chars"] = default_result_size_chars;
    j["turn_budget_chars"]         = turn_budget_chars;
    j["preview_size_chars"]        = preview_size_chars;

    if (tool_result_overrides.has_value()) {
        nlohmann::json m = nlohmann::json::object();
        for (auto& kv : *tool_result_overrides) m[kv.first] = kv.second;
        j["tool_result_overrides"] = std::move(m);
    } else {
        j["tool_result_overrides"] = nullptr;
    }

    if (extra_body.has_value()) j["extra_body"] = *extra_body;
    else j["extra_body"] = nullptr;
    return j;
}

std::vector<std::pair<std::string, std::string>>
HermesAgentEnvConfig::apply_terminal_env() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!terminal_backend.empty()) {
        set_env("TERMINAL_ENV", terminal_backend);
        out.emplace_back("TERMINAL_ENV", terminal_backend);
    }
    set_env("TERMINAL_TIMEOUT", std::to_string(terminal_timeout));
    out.emplace_back("TERMINAL_TIMEOUT", std::to_string(terminal_timeout));
    set_env("TERMINAL_LIFETIME_SECONDS", std::to_string(terminal_lifetime));
    out.emplace_back("TERMINAL_LIFETIME_SECONDS", std::to_string(terminal_lifetime));
    return out;
}

// ============================================================================
// Server-mode predicate
// ============================================================================
bool use_managed_server(const std::string& server_type) {
    // Phase 2 (ManagedServer) covers vLLM and SGLang server types.
    return server_type == "vllm" || server_type == "sglang"
        || server_type == "managed" || server_type == "vllm_managed";
}

// ============================================================================
// Trajectory display formatting
// ============================================================================
std::string format_trajectory_for_display(const nlohmann::json& messages) {
    if (!messages.is_array()) return "";
    std::vector<std::string> parts;
    for (const auto& msg : messages) {
        if (!msg.is_object()) continue;
        std::string role = msg.value("role", std::string{"unknown"});
        std::string content = msg.contains("content") && msg["content"].is_string()
            ? msg["content"].get<std::string>()
            : (msg.contains("content") && !msg["content"].is_null() ? msg["content"].dump() : "");

        if (role == "system") {
            parts.push_back("[SYSTEM]\n" + content);
        } else if (role == "user") {
            parts.push_back("[USER]\n" + content);
        } else if (role == "assistant") {
            std::string reasoning = msg.value("reasoning_content", std::string{});
            if (!reasoning.empty()) {
                parts.push_back("[ASSISTANT thinking]\n" + truncate(reasoning, 300));
            }
            if (!content.empty()) {
                parts.push_back("[ASSISTANT]\n" + content);
            }
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto& tc : msg["tool_calls"]) {
                    std::string name = "?";
                    std::string args = "{}";
                    if (tc.is_object() && tc.contains("function") && tc["function"].is_object()) {
                        if (tc["function"].contains("name") && tc["function"]["name"].is_string())
                            name = tc["function"]["name"].get<std::string>();
                        if (tc["function"].contains("arguments")) {
                            const auto& a = tc["function"]["arguments"];
                            args = a.is_string() ? a.get<std::string>() : a.dump();
                        }
                    }
                    parts.push_back("[TOOL CALL] " + name + "(" + truncate(args, 200) + ")");
                }
            }
        } else if (role == "tool") {
            parts.push_back("[TOOL RESULT] " + truncate(content, 500));
        }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n\n";
        out += parts[i];
    }
    return out;
}

std::string format_tool_error_summary(const nlohmann::json& err) {
    if (!err.is_object()) return "";
    int turn = err.value("turn", 0);
    std::string tool = err.value("tool", std::string{});
    std::string args = err.value("args", std::string{});
    std::string error = err.value("error", std::string{});
    std::ostringstream oss;
    oss << "[turn " << turn << "] " << tool << "("
        << trunc_no_ellipsis(args, 80) << ") -> "
        << trunc_no_ellipsis(error, 150);
    return oss.str();
}

std::string format_tool_error_details(const nlohmann::json& tool_errors) {
    if (!tool_errors.is_array()) return "";
    std::string out;
    for (size_t i = 0; i < tool_errors.size(); ++i) {
        if (i) out += "\n";
        out += format_tool_error_summary(tool_errors[i]);
    }
    return out;
}

}  // namespace hermes::environments::base_env
