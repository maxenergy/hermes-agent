// C++17 port of core pieces of hermes_cli/models.py:
//   - Provider catalogs (static snapshot of model IDs per provider).
//   - Provider labels + alias table.
//   - `hermes models [--provider NAME]` command that lists IDs with
//     optional context-window + baseline pricing info.
//   - Validation helpers: `normalize_provider`, `provider_label`,
//     `parse_model_input`, `detect_provider_for_model`.
//
// The live-catalog fetches (OpenRouter API, GitHub Models, Anthropic
// /v1/models, etc.) are intentionally left out — they need HTTPS client
// wiring.  The static snapshot is adequate for CLI listing and
// validation parity with the Python path.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::models_cmd {

// Returns the curated {model_id -> description} list for a provider.
// Empty vector if the provider is unknown.
struct CuratedModel {
    std::string id;
    std::string description;   // "recommended", "free", or "" for default
};
std::vector<CuratedModel> curated_models_for_provider(
    const std::string& provider);

// All canonical provider names (no aliases).  Sorted.
std::vector<std::string> known_providers();

// Is `provider` an alias or canonical name we recognize?
bool is_known_provider(const std::string& provider);

// Normalise alias -> canonical provider name.  Returns empty string if
// the provider is unknown (matches Python's `normalize_provider` which
// returns the input unchanged — but the bool `known_*` helpers let
// callers disambiguate).
std::string normalize_provider(const std::string& provider);

// Human-readable label for a provider.
std::string provider_label(const std::string& provider);

// Strip a `vendor/` prefix for cross-provider ID comparison.
std::string strip_vendor_prefix(const std::string& model_id);

// Split `provider:model` or `provider/model` into {provider, model}.
// When the input has no separator, `current_provider` is used as the
// provider and the input is returned verbatim as the model.
struct ParsedModel {
    std::string provider;
    std::string model;
};
ParsedModel parse_model_input(const std::string& raw,
                              const std::string& current_provider);

// Heuristic provider detection from a model ID.  Returns the first
// provider whose catalog contains the ID (with/without vendor prefix).
// Empty if no match.
std::string detect_provider_for_model(const std::string& model_id);

// Static context-window hints for the most-used IDs.  Returns 0 when
// unknown.
int context_window_for_model(const std::string& model_id);

// Baseline per-million-token pricing hints, returning `(prompt, completion)`
// in USD.  Returns (0,0) when unknown.
struct Pricing {
    double prompt_per_mtok = 0.0;
    double completion_per_mtok = 0.0;
};
Pricing pricing_for_model(const std::string& model_id);

// Fast-mode support + override resolution (mirrors
// model_supports_fast_mode / resolve_fast_mode_overrides).
bool model_supports_fast_mode(const std::string& model_id);

// `hermes models` — list models.  `--provider NAME` restricts listing
// to one provider; without it we list every provider.  Uses the static
// catalog; no network.
int run(int argc, char* argv[]);

}  // namespace hermes::cli::models_cmd
