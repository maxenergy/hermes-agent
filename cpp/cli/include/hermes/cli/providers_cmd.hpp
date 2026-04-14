// Single source of truth for provider identity (C++17 port of
// hermes_cli/providers.py).
//
// Three data sources, merged at runtime:
//   1. Hermes overlays (static table defined in providers_cmd.cpp)
//   2. models.dev catalog (not yet wired — returns empty when missing)
//   3. User config (providers: + custom_providers: sections)
//
// See the Python file for the original contract.  This module is pure:
// no I/O, no network, no global singletons that outlive a single call.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::providers_cmd {

// -- Hermes overlay ---------------------------------------------------------
// Hermes-specific provider metadata layered on top of models.dev.
struct HermesOverlay {
    std::string transport = "openai_chat";   // openai_chat | anthropic_messages | codex_responses
    bool is_aggregator = false;
    std::string auth_type = "api_key";       // api_key | oauth_device_code | oauth_external | external_process
    std::vector<std::string> extra_env_vars;
    std::string base_url_override;
    std::string base_url_env_var;
};

// The full overlay table — exposed for callers (model_switch_cmd) that walk
// every known overlay when searching for credentials.  Keyed by canonical
// (Hermes) provider id.
const std::unordered_map<std::string, HermesOverlay>& hermes_overlays();

// -- Resolved provider ------------------------------------------------------
// The merged result of overlays + user config (+ eventually models.dev).
struct ProviderDef {
    std::string id;
    std::string name;
    std::string transport = "openai_chat";
    std::vector<std::string> api_key_env_vars;
    std::string base_url;
    std::string base_url_env_var;
    bool is_aggregator = false;
    std::string auth_type = "api_key";
    std::string doc;
    std::string source;   // "hermes", "user-config"
};

// -- Aliases ----------------------------------------------------------------
// Maps human-friendly / legacy names → canonical provider ids.
const std::unordered_map<std::string, std::string>& aliases();

// -- Transport → API mode mapping ------------------------------------------
const std::unordered_map<std::string, std::string>& transport_to_api_mode();

// -- Helper functions -------------------------------------------------------

// Lower-case, trim, apply alias table.  Never returns empty for a non-empty
// input — aliases only fire on an exact canonical miss.
std::string normalize_provider(const std::string& name);

// Look up a provider by id/alias, merging overlay data.  Returns nullopt
// when the canonical id is unknown to the Hermes overlay table.
std::optional<ProviderDef> get_provider(const std::string& name);

// Display label for a provider; falls back to the canonical id when unknown.
std::string get_label(const std::string& provider_id);

// True when the provider is a multi-model aggregator (OpenRouter, Vercel AI
// Gateway, etc.).
bool is_aggregator(const std::string& provider);

// Determine the wire-protocol API mode for a provider/endpoint.  Falls back
// to URL heuristics for unknown/custom providers, then to
// "chat_completions" as a last resort.
std::string determine_api_mode(const std::string& provider,
                               const std::string& base_url = "");

// Resolve a provider from the user's ``providers:`` config block.  The
// input is the raw JSON/YAML as a nlohmann::json object.
std::optional<ProviderDef> resolve_user_provider(const std::string& name,
                                                 const nlohmann::json& user_config);

// Build the canonical slug for a ``custom_providers:`` entry
// (``custom:<normalised-name>``).  Matches the Python convention.
std::string custom_provider_slug(const std::string& display_name);

// Resolve a provider from the user's ``custom_providers:`` list.
std::optional<ProviderDef> resolve_custom_provider(const std::string& name,
                                                   const nlohmann::json& custom_providers);

// Full resolution chain: built-in (overlay) → user providers → custom
// providers.  The canonical entry point used by the --provider flag
// resolver.
std::optional<ProviderDef> resolve_provider_full(
    const std::string& name,
    const nlohmann::json& user_providers = nlohmann::json(),
    const nlohmann::json& custom_providers = nlohmann::json());

// List every provider known to the Hermes overlay table — for UI listings
// and credential probes.  Keys are canonical Hermes ids.
std::vector<std::string> list_overlay_providers();

// Extract the set of env-var names that *could* hold an API key for a
// provider (useful for "are we authed?" probes).  Returns an empty vector
// for unknown providers.
std::vector<std::string> api_key_env_vars_for(const std::string& provider);

}  // namespace hermes::cli::providers_cmd
