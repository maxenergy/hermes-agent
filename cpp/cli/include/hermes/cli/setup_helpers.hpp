// C++17 port of the pure-logic primitives that drive the
// `hermes setup` interactive wizard in `hermes_cli/setup.py`.
//
// Scope:
//   * Configuration mutators that translate user choices into JSON
//     edits (model defaults, credential pool strategies, reasoning
//     effort, agent settings).
//   * Per-section "configured-summary" inspection used by the
//     post-OpenClaw-migration skip flow (`_get_section_config_summary`,
//     `_skip_configured_section`).
//   * Default per-provider model lists (`_DEFAULT_PROVIDER_MODELS`) used
//     as fallback when the live `/v1/models` endpoint is unreachable.
//   * Discord user-id cleanup helper (`_clean_discord_user_ids`).
//   * Pretty-print helpers (`print_header`, `print_success`, etc.) and
//     the headless / non-interactive guidance message.
//
// Interactive prompts (curses menus, password input) and the OpenClaw
// migration entry point are intentionally left out — they require live
// terminal IO that the C++ entry point handles in `main_entry.cpp` and
// `curses_ui.cpp`.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::setup_helpers {

// ---------------------------------------------------------------------------
// Default per-provider model snapshots.
// ---------------------------------------------------------------------------

// Returns the default model list for `provider_id` (matches Python's
// `_DEFAULT_PROVIDER_MODELS`).  Returns an empty vector if the provider
// is unknown.
const std::vector<std::string>& default_models_for_provider(
    const std::string& provider_id);

// True when `provider_id` has an entry in the default-model table.
bool has_default_models(const std::string& provider_id);

// All providers with default-model tables, sorted alphabetically.
std::vector<std::string> providers_with_default_models();

// ---------------------------------------------------------------------------
// Model config dict helpers.
// ---------------------------------------------------------------------------

// Mirrors Python `_model_config_dict(config)`.  Returns a plain JSON
// object derived from `config["model"]`.  If the value is a string, it
// is hoisted into `{"default": "<that string>"}`.  Anything else maps
// to an empty object.
nlohmann::json model_config_dict(const nlohmann::json& config);

// Mirrors Python `_set_default_model(config, name)`.  Updates
// `config["model"]["default"]` in-place.  No-op when `model_name` is
// empty.
void set_default_model(nlohmann::json& config, const std::string& model_name);

// ---------------------------------------------------------------------------
// Credential pool strategies.
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::string>
get_credential_pool_strategies(const nlohmann::json& config);

void set_credential_pool_strategy(nlohmann::json& config,
                                  const std::string& provider,
                                  const std::string& strategy);

// True when the provider supports same-provider pool setup.  Matches
// `_supports_same_provider_pool_setup` — `openrouter` always wins; any
// provider with auth_type ∈ {api_key, oauth_device_code} also wins.
// `auth_type_lookup` is injected so the helper stays free of the
// (heavy) provider registry; pass a callable that returns the auth
// type string for a given provider (empty string when unknown).
using AuthTypeLookup =
    std::function<std::string(const std::string& provider_id)>;
bool supports_same_provider_pool_setup(const std::string& provider,
                                       const AuthTypeLookup& auth_type_lookup);

// ---------------------------------------------------------------------------
// Reasoning effort.
// ---------------------------------------------------------------------------

// Mirrors Python `_current_reasoning_effort(config)` — lower-cases and
// strips the value at `config["agent"]["reasoning_effort"]`.  Returns
// empty string when missing or not a string.
std::string current_reasoning_effort(const nlohmann::json& config);

// Mirrors Python `_set_reasoning_effort(config, effort)`.  Creates the
// `agent` sub-object on demand.
void set_reasoning_effort(nlohmann::json& config, const std::string& effort);

// Compute the reasoning-effort selection menu derived from the current
// config + the provider's available efforts list.  Returns the rendered
// choice strings and the index of the default.  Mirrors the bookkeeping
// in `_setup_copilot_reasoning_selection`.
struct ReasoningChoiceMenu {
    std::vector<std::string> choices;
    int default_index = 0;
};
ReasoningChoiceMenu build_reasoning_choice_menu(
    const nlohmann::json& config, const std::vector<std::string>& efforts);

// Apply the user's selected index to the config.  `efforts.size()` ==
// "Disable reasoning"; anything beyond that is "keep current" — no-op.
void apply_reasoning_choice(nlohmann::json& config,
                            const std::vector<std::string>& efforts,
                            int selected_index);

// ---------------------------------------------------------------------------
// Section "already configured" summary.
// ---------------------------------------------------------------------------

// Function signature used to look up environment variables.  Mirrors
// the Python `get_env_value` import indirection so tests can stub.
using EnvLookupFn = std::function<std::string(const std::string& name)>;

// Mirrors `_get_section_config_summary`.  `section_key` ∈ {model,
// terminal, agent, gateway, tools}.  Returns nullopt when the section
// is not configured (must run); otherwise returns the human-readable
// summary string.  `active_provider_lookup` may be empty — a missing
// callback is treated as "no active provider".
using ActiveProviderLookup = std::function<std::optional<std::string>()>;
std::optional<std::string> get_section_config_summary(
    const nlohmann::json& config,
    const std::string& section_key,
    const EnvLookupFn& env_lookup,
    const ActiveProviderLookup& active_provider_lookup = {});

// ---------------------------------------------------------------------------
// Discord user-id cleanup.
// ---------------------------------------------------------------------------

// Strip Discord mention prefixes (`<@`, `<@!`, `>`, `user:`) from a
// comma-separated user-id list.  Mirrors `_clean_discord_user_ids`.
std::vector<std::string> clean_discord_user_ids(const std::string& raw);

// ---------------------------------------------------------------------------
// Pretty-print helpers.
// ---------------------------------------------------------------------------

// Format helpers that render to a string instead of stdout so callers
// can buffer output for tests.  All helpers honour TTY detection in
// `colors.hpp`.
std::string format_header(const std::string& title);
std::string format_info(const std::string& text);
std::string format_success(const std::string& text);
std::string format_warning(const std::string& text);
std::string format_error(const std::string& text);

void print_header(const std::string& title);
void print_info(const std::string& text);
void print_success(const std::string& text);
void print_warning(const std::string& text);
void print_error(const std::string& text);

// True when stdin is connected to a TTY.  Mirrors `is_interactive_stdin`.
bool is_interactive_stdin();

// Render the headless guidance message into a string.  Mirrors
// `print_noninteractive_setup_guidance`.  `reason` may be empty.
std::string format_noninteractive_setup_guidance(const std::string& reason);

void print_noninteractive_setup_guidance(const std::string& reason = {});

}  // namespace hermes::cli::setup_helpers
