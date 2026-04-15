// C++17 port of pure-logic helpers from `hermes_cli/runtime_provider.py`.
// No network / subprocess / config-file I/O — everything takes explicit
// inputs so tests can drive the full state space.
#pragma once

#include <optional>
#include <string>

namespace hermes::cli::runtime_provider_helpers {

// ---------------------------------------------------------------------------
// Defaults (mirrors the constants from auth.py / hermes_constants).
// ---------------------------------------------------------------------------

inline constexpr const char* kDefaultCodexBaseUrl
    = "https://api.openai.com/v1";
inline constexpr const char* kDefaultQwenBaseUrl
    = "https://portal.qwen.ai/v1";
inline constexpr const char* kDefaultAnthropicBaseUrl
    = "https://api.anthropic.com";
inline constexpr const char* kDefaultOpenRouterBaseUrl
    = "https://openrouter.ai/api/v1";

// ---------------------------------------------------------------------------
// String helpers.
// ---------------------------------------------------------------------------

// Lowercase + trim + replace inner spaces with dashes.
// Mirrors `_normalize_custom_provider_name`.
std::string normalize_custom_provider_name(const std::string& value);

// Mirrors `_detect_api_mode_for_url`.  Returns "codex_responses" when the
// URL is a direct api.openai.com endpoint (not OpenRouter); std::nullopt
// otherwise.
std::optional<std::string> detect_api_mode_for_url(const std::string& base_url);

// Validate an api_mode value.  Returns the normalised form if valid,
// std::nullopt otherwise.  Accepted values:
// "chat_completions", "codex_responses", "anthropic_messages".
std::optional<std::string> parse_api_mode(const std::string& raw);

// True if `base_url` targets a localhost / 127.0.0.1 endpoint.
bool is_local_base_url(const std::string& base_url);

// True when the persisted api_mode should be honored for a given runtime
// provider.  Mirrors `_provider_supports_explicit_api_mode`.
bool provider_supports_explicit_api_mode(
    const std::string& provider,
    const std::string& configured_provider);

// ---------------------------------------------------------------------------
// Defaults per provider.
// ---------------------------------------------------------------------------

// Return the default base URL for a given provider (or empty string if
// the provider doesn't define one).
std::string default_base_url_for_provider(const std::string& provider);

// Return the default api_mode for a given provider.  For providers that
// depend on the URL (e.g. "custom"), returns "chat_completions".
std::string default_api_mode_for_provider(const std::string& provider);

// ---------------------------------------------------------------------------
// Base-URL normalisation.
// ---------------------------------------------------------------------------

// Strip a single trailing slash (if any).
std::string strip_trailing_slash(const std::string& url);

// Strip trailing "/v1" or "/v1/" used by OpenCode endpoints when we need
// to call the Anthropic messages API (so the SDK can append /v1/messages).
std::string strip_v1_suffix(const std::string& url);

// True when `base_url` ends with "/anthropic" — hint for anthropic_messages.
bool url_hints_anthropic_mode(const std::string& base_url);

// ---------------------------------------------------------------------------
// Requested-provider resolution.
// ---------------------------------------------------------------------------

// Resolve the user-requested provider given explicit CLI flag, config
// setting, and env var.  Returns the lower-cased name or "auto".
//
// `requested` — --provider flag value (may be empty).
// `cfg_provider` — model.provider from config.yaml (may be empty).
// `env_provider` — HERMES_INFERENCE_PROVIDER env var (may be empty).
std::string resolve_requested_provider(
    const std::string& requested,
    const std::string& cfg_provider,
    const std::string& env_provider);

// ---------------------------------------------------------------------------
// Error-message formatting.
// ---------------------------------------------------------------------------

// Format a runtime-provider error for the CLI:
//   * maps known error types to user-friendly messages
//   * strips ANSI escapes from the inner message.
// Pure: all state passed as arguments.
struct ErrorContext {
    std::string provider;      // e.g. "openai-codex"
    std::string error_class;   // e.g. "AuthError", "ValueError"
    std::string message;       // raw exception message
};
std::string format_runtime_provider_error(const ErrorContext& ctx);

// ---------------------------------------------------------------------------
// Pool-entry resolution — decision table for
// `_resolve_runtime_from_pool_entry`.
// ---------------------------------------------------------------------------

// Subset of `PooledCredential` needed for the runtime resolution math.
// Only string-valued inputs — no live pool reference.
struct PoolEntry {
    std::string runtime_base_url;
    std::string base_url;
    std::string runtime_api_key;
    std::string access_token;
    std::string source;
};

// Subset of the model-config dict.
struct ModelConfig {
    std::string provider;
    std::string base_url;
    std::string api_mode;
    std::string default_model;
};

// The resolved runtime dictionary.
struct ResolvedRuntime {
    std::string provider;
    std::string api_mode;
    std::string base_url;
    std::string api_key;
    std::string source;
    std::string requested_provider;
};

// Resolve the runtime dict for a pool entry.  Mirrors
// `_resolve_runtime_from_pool_entry`.
//
// This is the pure decision-table: it assumes the pool, registry and
// Copilot-model classifier have already been consulted by the caller and
// encoded into `model_cfg`.
ResolvedRuntime resolve_runtime_from_pool_entry(
    const std::string& provider,
    const PoolEntry& entry,
    const std::string& requested_provider,
    const ModelConfig& model_cfg);

// ---------------------------------------------------------------------------
// Custom-provider name extraction.
// ---------------------------------------------------------------------------

// Extract the name portion from a "custom:<name>" provider label.
// Returns empty string if the label is not in that form.
std::string extract_custom_provider_name(const std::string& label);

// True when the provider string is one of the pool-backed providers
// that must load credentials from an external pool.
bool is_pool_backed_provider(const std::string& provider);

}  // namespace hermes::cli::runtime_provider_helpers
