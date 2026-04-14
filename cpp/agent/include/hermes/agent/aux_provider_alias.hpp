// Auxiliary-provider alias normalisation.
//
// C++17 port of the small helper half of agent/auxiliary_client.py:
//   _PROVIDER_ALIASES, _normalize_aux_provider(). Lets consumers resolve
// "google-gemini" → "gemini", "moonshot" → "kimi-coding", etc., without
// pulling in the full OpenAI SDK routing chain.
#pragma once

#include <optional>
#include <string>

namespace hermes::agent::aux {

// Return the canonical name for a provider string. "auto"/""/null map
// to "auto". Unknown providers are returned unchanged (lower-cased).
//
// When `for_vision=true`, "custom:<suffix>" collapses to "custom"
// (vision routing uses a different resolution chain that does not
// support named custom providers).
//
// The special sentinel "main" requires the caller to resolve the user's
// actual main provider — the function leaves "main" in place and
// returns it verbatim. Callers substitute the actual provider before
// calling if available.
std::string normalize_provider(const std::string& provider,
                               bool for_vision = false);

// Lookup the alias target for `provider` (lower-cased). Returns
// std::nullopt when the provider is not aliased.
std::optional<std::string> alias_target(const std::string& provider);

// Default auxiliary model slug for each direct-API provider family,
// matching Python's AUXILIARY_DEFAULT_MODELS table. Returns an empty
// string when the provider has no known default.
std::string default_model_for(const std::string& provider);

}  // namespace hermes::agent::aux
