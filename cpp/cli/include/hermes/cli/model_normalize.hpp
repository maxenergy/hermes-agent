// C++17 port of `hermes_cli/model_normalize.py` -- per-provider model
// name normalisation.
//
// Different LLM providers expect model identifiers in different formats:
//   * Aggregators (OpenRouter, Nous, AI Gateway, Kilo Code) expect
//     `vendor/model` slugs such as `anthropic/claude-sonnet-4.6`.
//   * Anthropic and OpenCode-Zen expect bare names with dots replaced by
//     hyphens (`claude-sonnet-4-6`).
//   * Copilot (including `copilot-acp`) expects bare names with dots
//     preserved (`claude-sonnet-4.6`).
//   * DeepSeek only accepts two identifiers: `deepseek-chat` and
//     `deepseek-reasoner`.
//   * Authoritative native providers (Gemini, HuggingFace, Codex) pass
//     through unchanged.
//   * Direct providers such as `zai`, `kimi-coding`, `minimax`,
//     `minimax-cn`, `alibaba`, `qwen-oauth`, `custom` strip a
//     `provider/` prefix only when it matches the target provider.
//
// This module centralises that translation so callers can write::
//
//     auto api_model = normalize_model_for_provider(user_input, provider);
//
// The port mirrors the Python implementation verbatim; the sole
// dependency on `hermes_cli.models.normalize_provider` is replaced with
// a pluggable `provider_alias_resolver_t` that defaults to lowercasing
// the raw input.
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace hermes::cli::model_normalize {

// Functor used to resolve provider aliases (e.g. `anthropics` ->
// `anthropic`).  Tests inject a deterministic resolver; production
// callers may plug in the richer table from `hermes::cli::models`.
using provider_alias_resolver_t = std::function<std::string(const std::string&)>;

// Install a custom provider alias resolver.  Pass `nullptr` to reset to
// the default "trim + lowercase" behaviour.
void set_provider_alias_resolver(provider_alias_resolver_t resolver);

// Detect the vendor slug for a bare model name.
//
// Examples:
//   detect_vendor("claude-sonnet-4.6")          -> "anthropic"
//   detect_vendor("gpt-5.4-mini")               -> "openai"
//   detect_vendor("anthropic/claude-sonnet-4.6") -> "anthropic"
//   detect_vendor("my-custom-model")            -> std::nullopt
std::optional<std::string> detect_vendor(const std::string& model_name);

// Strip a `vendor/` prefix if present.
std::string strip_vendor_prefix(const std::string& model_name);

// Replace dots with hyphens.
std::string dots_to_hyphens(const std::string& model_name);

// Prepend the detected `vendor/` prefix if missing.  Returns the name
// unchanged when a vendor cannot be detected or a `/` is already
// present.
std::string prepend_vendor(const std::string& model_name);

// Strip `provider/` only when the prefix matches the target provider
// (after alias resolution).  Prevents slash-bearing IDs from being
// mangled on native providers while still repairing manual config
// values such as `zai/glm-5.1` for the `zai` provider.
std::string strip_matching_provider_prefix(const std::string& model_name,
                                           const std::string& target_provider);

// Map any model input to one of DeepSeek's two accepted identifiers.
// Exposed for testing; normally called by
// `normalize_model_for_provider`.
std::string normalize_for_deepseek(const std::string& model_name);

// Main entry point -- translate a model name into the format the target
// provider's API expects.  Mirrors the Python docstring semantics
// exactly; see the header overview for provider classifications.
std::string normalize_model_for_provider(const std::string& model_input,
                                         const std::string& target_provider);

// Test/introspection helpers -- expose the classification frozensets so
// tests can assert the taxonomy without hard-coding duplicates.
bool is_aggregator_provider(const std::string& canonical_provider);
bool is_dot_to_hyphen_provider(const std::string& canonical_provider);
bool is_strip_vendor_only_provider(const std::string& canonical_provider);
bool is_authoritative_native_provider(const std::string& canonical_provider);
bool is_matching_prefix_strip_provider(const std::string& canonical_provider);

}  // namespace hermes::cli::model_normalize
