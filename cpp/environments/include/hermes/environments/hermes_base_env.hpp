// Portable surface of environments/hermes_base_env.py.
//
// The Atropos integration (BaseEnv subclassing, async loop coordination,
// ServerManager wiring, tokenizer access) is intentionally NOT ported —
// those depend on the Python atroposlib runtime.  The C++ port covers the
// pieces that an embedded backend can actually use:
//
//   * HermesAgentEnvConfig — the typed configuration block + validators.
//     Mirrors every field of the Python pydantic model with the same
//     defaults, so YAML/JSON config files round-trip identically.
//   * BudgetConfig builder (matches build_budget_config()).
//   * Trajectory display formatter (_format_trajectory_for_display) used
//     by wandb logging — pure string formatting, easy to test.
//   * Tool-error summary builder (the wandb_log() inner block).
//   * Server-mode detection helper (Phase 1 vs Phase 2) — pure predicate
//     over a server-type string.
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::environments::base_env {

// Default budget knobs lifted from tools.budget_config (kept in sync with
// the Python defaults the env config inherits).
inline constexpr int kDefaultResultSizeChars = 25'000;
inline constexpr int kDefaultTurnBudgetChars = 60'000;
inline constexpr int kDefaultPreviewSizeChars = 2'000;

// Validation problem reported by HermesAgentEnvConfig::validate().
struct ConfigError {
    std::string field;
    std::string message;
};

// Mirror of the BudgetConfig produced by the Python env's build_budget_config().
struct BudgetConfig {
    int default_result_size = kDefaultResultSizeChars;
    int turn_budget         = kDefaultTurnBudgetChars;
    int preview_size        = kDefaultPreviewSizeChars;
    std::unordered_map<std::string, int> tool_overrides;

    nlohmann::json to_json() const;
};

// Field-for-field mirror of HermesAgentEnvConfig.  Values are stored as
// JSON-friendly types so the struct round-trips through YAML/JSON loaders.
struct HermesAgentEnvConfig {
    // Toolset configuration (mutually exclusive: enabled_toolsets ⊕ distribution)
    std::optional<std::vector<std::string>> enabled_toolsets;
    std::optional<std::vector<std::string>> disabled_toolsets;
    std::optional<std::string> distribution;

    // Agent loop configuration
    int max_agent_turns = 30;
    std::optional<std::string> system_prompt;
    double agent_temperature = 1.0;

    // Terminal backend
    std::string terminal_backend = "local";
    int terminal_timeout = 120;
    int terminal_lifetime = 3600;

    // Dataset
    std::optional<std::string> dataset_name;
    std::string dataset_split = "train";
    std::string prompt_field  = "prompt";

    // Thread pool
    int tool_pool_size = 128;

    // Tool call parser (Phase 2)
    std::string tool_call_parser = "hermes";

    // Tool result budget
    int default_result_size_chars = kDefaultResultSizeChars;
    int turn_budget_chars         = kDefaultTurnBudgetChars;
    int preview_size_chars        = kDefaultPreviewSizeChars;
    std::optional<std::unordered_map<std::string, int>> tool_result_overrides;

    // Provider-specific extra body (passed to the OpenAI client)
    std::optional<nlohmann::json> extra_body;

    // ----------------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------------
    BudgetConfig build_budget_config() const;
    std::vector<ConfigError> validate() const;

    // JSON round-trip helpers (matches the pydantic model_dump shape).
    static HermesAgentEnvConfig from_json(const nlohmann::json& j);
    nlohmann::json to_json() const;

    // Bake terminal_backend / terminal_timeout / terminal_lifetime into the
    // env vars hermes tools read at runtime.  Returns the {key, value} pairs
    // that were set so callers can audit / unset them in tests.
    std::vector<std::pair<std::string, std::string>> apply_terminal_env() const;
};

// True when the server should drive Phase 2 (ManagedServer / vLLM / SGLang).
// Pure predicate over the server type string — mirrors the Python
// `not isinstance(server, OpenAIServer)` check.
bool use_managed_server(const std::string& server_type);

// Format the conversation trajectory for wandb display.  Pure string-building
// from the OpenAI-format `messages` array.  Truncates assistant reasoning to
// 300 chars, tool-call args to 200 chars, and tool results to 500 chars.
std::string format_trajectory_for_display(const nlohmann::json& messages);

// Build the per-error summary line wandb_log() emits, matching:
//   "[turn N] tool(args[:80]) -> error[:150]"
std::string format_tool_error_summary(const nlohmann::json& tool_error);

// Build the multi-line "tool_error_details" string from a list of error
// dicts (each shaped like {turn, tool, args, error, tool_result}).
std::string format_tool_error_details(const nlohmann::json& tool_errors);

}  // namespace hermes::environments::base_env
