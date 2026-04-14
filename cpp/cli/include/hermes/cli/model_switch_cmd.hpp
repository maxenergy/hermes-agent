// C++17 port of hermes_cli/model_switch.py.
//
// Shared model-switching pipeline used by the CLI `/model` command and the
// gateway.  Resolves aliases, normalises model names, and builds a
// ModelSwitchResult that the caller can persist.
//
// This is a pure-logic port: it does not touch the credential pool, session
// store, or any other IO-bound subsystem.  Callers supply the current
// provider/model/base_url/api_key, and receive a target provider + model id.
#pragma once

#include "hermes/cli/providers_cmd.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace hermes::cli::model_switch_cmd {

// Warning emitted when the caller switches to a Nous Research Hermes LLM
// (not an agent model — lacks tool-calling).  Exported so tests can check
// the exact text.
extern const char* const HERMES_MODEL_WARNING;

std::string check_hermes_model_warning(const std::string& model_name);

// -- Alias tables -----------------------------------------------------------

struct ModelIdentity {
    std::string vendor;
    std::string family;
};

// Short-name aliases resolved against the current provider's catalog.
// These have NO version numbers — the catalog walk picks the newest match.
const std::unordered_map<std::string, ModelIdentity>& model_aliases();

struct DirectAlias {
    std::string model;
    std::string provider;
    std::string base_url;
};

// User-config direct aliases (model + provider + base_url).  Loaded from
// the supplied config JSON; built-in set is empty.
std::unordered_map<std::string, DirectAlias> load_direct_aliases(
    const nlohmann::json& config);

// -- Flag parsing ----------------------------------------------------------

struct ModelFlags {
    std::string model_input;
    std::string explicit_provider;
    bool is_global = false;
};

ModelFlags parse_model_flags(const std::string& raw_args);

// -- Alias resolution -------------------------------------------------------

struct AliasResolution {
    std::string provider;
    std::string model;
    std::string alias_name;
};

// Resolve *raw_input* against the current provider's model catalog.
// *catalog* is the list of model IDs known for *current_provider* (a
// caller-supplied slice of models.dev).  Returns nullopt when no match.
std::optional<AliasResolution> resolve_alias(
    const std::string& raw_input,
    const std::string& current_provider,
    const std::vector<std::string>& catalog,
    const std::unordered_map<std::string, DirectAlias>& direct_aliases = {});

// Fallback: try alias resolution on each authed provider in turn.
// *catalogs_by_provider* maps provider slug → its model list.
std::optional<AliasResolution> resolve_alias_fallback(
    const std::string& raw_input,
    const std::vector<std::string>& authenticated_providers,
    const std::unordered_map<std::string, std::vector<std::string>>& catalogs_by_provider,
    const std::unordered_map<std::string, DirectAlias>& direct_aliases = {});

// -- Model switch result ----------------------------------------------------

struct ModelSwitchResult {
    bool success = false;
    std::string new_model;
    std::string target_provider;
    bool provider_changed = false;
    std::string api_key;
    std::string base_url;
    std::string api_mode;
    std::string error_message;
    std::string warning_message;
    std::string provider_label;
    std::string resolved_via_alias;
    bool is_global = false;
};

// Inputs to switch_model() — fold the many optional parameters to a struct
// to keep the call site readable.
struct ModelSwitchInputs {
    std::string raw_input;
    std::string current_provider;
    std::string current_model;
    std::string current_base_url;
    std::string current_api_key;
    bool is_global = false;
    std::string explicit_provider;
    nlohmann::json user_providers;    // object or null
    nlohmann::json custom_providers;  // array or null
    // Caller-supplied catalog: provider slug → list of model ids.  Used for
    // alias resolution and aggregator lookup.  Missing entries are treated
    // as "no catalog available".
    std::unordered_map<std::string, std::vector<std::string>> catalogs;
    // Pre-loaded direct aliases (merged from config).  May be empty.
    std::unordered_map<std::string, DirectAlias> direct_aliases;
};

// Pure resolution — does NOT touch credentials.  Callers that need a fully
// resolved (api_key + base_url) result must layer a credential-pool lookup
// on top, using `result.target_provider`.
ModelSwitchResult switch_model(const ModelSwitchInputs& in);

// Helper used by both switch_model and gateway tooling — normalises a
// model name for a target provider (strip `openrouter/` prefix,
// lower-case, etc).  The Python helper is provider-specific but the
// default rules are:
//   * If the provider is an aggregator and the model lacks "/", keep as-is.
//   * If the provider is non-aggregator and the model contains a slash,
//     strip the vendor prefix.
std::string normalize_model_for_provider(const std::string& model,
                                         const std::string& provider);

// Aggregator catalog search: given a free-form model name and an
// aggregator's catalog (list of "vendor/model" slugs), try to match by
// exact slug or bare model name.  Returns the matched full slug or the
// original input when no match.
std::string aggregator_catalog_lookup(const std::string& model,
                                      const std::vector<std::string>& catalog);

// On aggregators, convert `vendor:model` → `vendor/model` when there is no
// slash in the input — colons are reserved for variant suffixes on
// already-slashed names.
std::string convert_vendor_colon_to_slash(const std::string& raw_input,
                                          bool is_aggregator);

// Tier-down fallback chain: given a primary provider, return the list of
// providers to try in order (primary, then the user's authenticated set).
// De-duplicates while preserving order.
std::vector<std::string> fallback_chain(
    const std::string& primary,
    const std::vector<std::string>& authenticated_providers);

}  // namespace hermes::cli::model_switch_cmd
