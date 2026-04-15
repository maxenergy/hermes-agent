// C++17 port of additional helpers from `hermes_cli/models.py`.
//
// Layered on top of `models_cmd` to keep that file focused on the
// `hermes models` subcommand wiring.  This header covers:
//
//   * Fast-mode / Priority-Processing model classification +
//     request-overrides resolution.
//   * Anthropic fast-mode classification (separate predicate).
//   * Free-model detection (`_openrouter_model_is_free`,
//     `_is_model_free`).
//   * Nous Portal allowlist filter (`filter_nous_free_models`,
//     `partition_nous_models_by_tier`, `is_nous_free_tier`).
//   * Per-million-token price formatter (`_format_price_per_mtok`).
//   * Generic `_payload_items` JSON normaliser used by every API
//     catalog parser.
//   * OpenRouter-slug lookup for bare model names.
//   * Curated OpenRouter snapshot list (`OPENROUTER_MODELS`).
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::cli::models_helpers {

// ---------------------------------------------------------------------------
// Static OpenRouter snapshot.
// ---------------------------------------------------------------------------

// `(model_id, description)` tuples — verbatim copy of `OPENROUTER_MODELS`.
// Description is "recommended", "free", or "" for default rows.
const std::vector<std::pair<std::string, std::string>>&
openrouter_snapshot();

// ---------------------------------------------------------------------------
// Vendor-prefix helpers.
// ---------------------------------------------------------------------------

// Strip the `vendor/` prefix from a model ID (lower-cased).  Mirrors
// `_strip_vendor_prefix`.
std::string strip_vendor_prefix(const std::string& model_id);

// ---------------------------------------------------------------------------
// Fast-mode classification.
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& priority_processing_models();
const std::unordered_set<std::string>& anthropic_fast_mode_models();

// True when the model supports OpenAI Priority Processing OR Anthropic
// Fast Mode.  Mirrors `model_supports_fast_mode`.
bool model_supports_fast_mode(const std::string& model_id);

// True only for the Anthropic fast-mode subset.  Mirrors
// `_is_anthropic_fast_model`.
bool is_anthropic_fast_model(const std::string& model_id);

// Returns the request-override JSON object to inject into request
// kwargs, or nullopt when fast mode is unsupported for this model.
//   * Anthropic models  → `{"speed": "fast"}`
//   * OpenAI / Codex    → `{"service_tier": "priority"}`
// Mirrors `resolve_fast_mode_overrides`.
std::optional<nlohmann::json> resolve_fast_mode_overrides(
    const std::string& model_id);

// ---------------------------------------------------------------------------
// Free-model detection (OpenRouter / Nous pricing).
// ---------------------------------------------------------------------------

// True when the pricing JSON has prompt == 0 AND completion == 0.
// Non-object inputs return false.  Mirrors `_openrouter_model_is_free`.
bool openrouter_model_is_free(const nlohmann::json& pricing);

// True when the pricing map has a zero-cost entry for `model_id`.
// Mirrors `_is_model_free`.
bool is_model_free(const std::string& model_id, const nlohmann::json& pricing_map);

// ---------------------------------------------------------------------------
// Nous Portal tier helpers.
// ---------------------------------------------------------------------------

// Models allowed to appear when free on Nous Portal.  Mirrors
// `_NOUS_ALLOWED_FREE_MODELS`.
const std::unordered_set<std::string>& nous_allowed_free_models();

// Apply the free-model policy — mirrors `filter_nous_free_models`.
std::vector<std::string> filter_nous_free_models(
    const std::vector<std::string>& model_ids,
    const nlohmann::json& pricing_map);

// Mirrors `is_nous_free_tier(account_info)`.  Empty / non-object input
// → false (assume paid).  `monthly_charge == 0` → true.
bool is_nous_free_tier(const nlohmann::json& account_info);

// Split into (selectable, unavailable).  Mirrors
// `partition_nous_models_by_tier`.
std::pair<std::vector<std::string>, std::vector<std::string>>
partition_nous_models_by_tier(const std::vector<std::string>& model_ids,
                              const nlohmann::json& pricing_map,
                              bool free_tier);

// ---------------------------------------------------------------------------
// Pricing formatter.
// ---------------------------------------------------------------------------

// Format a per-token cost string as `$<price>.<dd>` per million tokens.
// Returns "free" when zero, "?" when un-parseable.  Mirrors
// `_format_price_per_mtok` (always 2 decimal places).
std::string format_price_per_mtok(const std::string& per_token_str);

// ---------------------------------------------------------------------------
// JSON helpers.
// ---------------------------------------------------------------------------

// Normalise a `/v1/models`-style payload into a vector of object items.
// Accepts either `{"data": [...]}` or a bare array.  Non-object items
// are dropped.  Mirrors `_payload_items`.
std::vector<nlohmann::json> payload_items(const nlohmann::json& payload);

// ---------------------------------------------------------------------------
// OpenRouter slug lookup.
// ---------------------------------------------------------------------------

// Find the full OpenRouter slug for `model_name`.  Tries an exact
// (case-insensitive) match against the catalog first, then matches the
// part after the `/`.  Returns nullopt when no match.
//
// `catalog` defaults to `openrouter_snapshot()` when empty.  Mirrors
// `_find_openrouter_slug`.
std::optional<std::string> find_openrouter_slug(
    const std::string& model_name,
    const std::vector<std::string>& catalog = {});

}  // namespace hermes::cli::models_helpers
