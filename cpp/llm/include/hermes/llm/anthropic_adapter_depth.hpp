// Anthropic adapter — depth port of agent/anthropic_adapter.py.
//
// Covers the request-shaping and response-normalisation logic that lives
// between the raw wire format (anthropic_client.cpp) and the feature
// helpers (anthropic_features.hpp).  All helpers are pure and I/O-free.
//
//   * convert_openai_tools_to_anthropic — function schema translation
//   * convert_openai_messages_to_anthropic — multi-role message rewriter
//       · extracts system prompt (string OR cache-control-aware array)
//       · merges consecutive tool_result user blocks
//       · strips orphaned tool_use / tool_result pairs
//       · enforces strict role alternation
//       · thinking-block signature management (third-party vs. native)
//   * build_anthropic_kwargs — full request body assembly
//       · OAuth Claude Code identity (system prefix, mcp_ tool prefix,
//         content-filter sanitization)
//       · adaptive vs. manual thinking, temperature clamp, max_tokens
//         extension when budget > current cap
//       · tool_choice mapping, fast-mode plumbing, extra_headers
//   * normalize_anthropic_response — wire JSON → provider-agnostic shape
//   * max output token lookup — per-model ceiling (Opus 4.6 = 128K, etc.)
//   * model name normalisation (anthropic/ prefix strip, dot-to-hyphen)
//   * OAuth token detection & refresh payload shape helpers
//   * PKCE code_verifier / code_challenge generation
//
// Anthropic-proprietary thinking block signatures are stripped when the
// endpoint is third-party compatible (MiniMax, Azure Foundry, etc.).
#pragma once

#include "hermes/llm/message.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hermes::llm {

// ── max output tokens table ─────────────────────────────────────────────

/// Return the per-model native output-token ceiling.  Uses longest-prefix
/// substring matching so date-stamped IDs (claude-opus-4-6-20260101) and
/// variant suffixes (:1m, :fast) resolve correctly.  Model strings are
/// lower-cased and dots are normalised to hyphens before matching.
int get_anthropic_max_output_tokens(std::string_view model);

// ── model name normalisation ────────────────────────────────────────────

/// Strip the OpenRouter "anthropic/" prefix (case-insensitive) and convert
/// dots in version numbers to hyphens unless preserve_dots is true.
///   "anthropic/claude-opus-4.6" → "claude-opus-4-6"
///   "claude-opus-4.6" (preserve_dots) → "claude-opus-4.6"
std::string normalize_anthropic_model_name(std::string_view model,
                                           bool preserve_dots = false);

/// Anthropic tool-call IDs must match [a-zA-Z0-9_-].  Replace invalid
/// characters with underscores and fall back to "tool_0" for empties.
std::string sanitize_anthropic_tool_id(std::string_view tool_id);

// ── tool / content conversion ───────────────────────────────────────────

/// Convert an OpenAI tools array to the Anthropic tools array shape:
///   [{"type":"function","function":{"name","description","parameters"}}]
/// →
///   [{"name","description","input_schema"}]
nlohmann::json convert_openai_tools_to_anthropic(const nlohmann::json& tools);

/// Convert a single OpenAI content part to the Anthropic content block.
///   "string"          → {"type":"text","text":"string"}
///   {"type":"input_text","text":…}     → {"type":"text","text":…}
///   {"type":"image_url","image_url":…} → {"type":"image","source":…}
///   {"type":"input_image","image_url":…} → same as above
/// Preserves cache_control when present.  Returns json::object() for null.
nlohmann::json convert_openai_content_part(const nlohmann::json& part);

/// Convert an "image_url" string (possibly a data-URL) to an Anthropic
/// image source object:
///   "data:image/png;base64,…" → {"type":"base64","media_type":…,"data":…}
///   "https://…"               → {"type":"url","url":"…"}
nlohmann::json image_source_from_openai_url(std::string_view url);

// ── message conversion ──────────────────────────────────────────────────

struct ConvertedMessages {
    /// Either a plain string, a JSON array of content blocks (when any
    /// system block carried cache_control), or `nullptr` (no system).
    nlohmann::json system = nullptr;
    nlohmann::json messages = nlohmann::json::array();
};

/// Convert an OpenAI-format messages array to the Anthropic shape.
///
/// Key rules:
///   * system messages are extracted and returned separately.  When any
///     system block declares cache_control, system is returned as an
///     array; otherwise a string is returned.
///   * assistant messages emit preserved thinking blocks first, then
///     content, then tool_use blocks (tool_calls in OpenAI speak).
///   * tool role messages become tool_result user blocks; consecutive
///     tool results merge into a single user message.
///   * orphaned tool_use blocks (no matching tool_result) are stripped,
///     and vice versa.
///   * strict role alternation is enforced — consecutive same-role
///     messages merge.
///   * thinking-block signatures are managed:
///       · third-party endpoints → all thinking blocks stripped
///       · native Anthropic → keep signed blocks on latest assistant,
///         strip from earlier assistants
ConvertedMessages convert_openai_messages_to_anthropic(
    const nlohmann::json& messages,
    std::string_view base_url = "");

// ── request builder ─────────────────────────────────────────────────────

struct AnthropicBuildOptions {
    std::string model;
    nlohmann::json messages;
    nlohmann::json tools = nullptr;               // OpenAI tools array or null
    std::optional<int> max_tokens;                // None → use native ceiling
    nlohmann::json reasoning_config = nullptr;    // {enabled, effort}
    std::optional<std::string> tool_choice;       // "auto"|"required"|"none"|name
    bool is_oauth = false;
    bool preserve_dots = false;
    std::optional<int> context_length;            // clamp output cap to fit
    std::string base_url;
    bool fast_mode = false;
};

/// Build the kwargs object for an Anthropic messages.create() call.
/// See anthropic_adapter.py::build_anthropic_kwargs for the contract.
nlohmann::json build_anthropic_kwargs(const AnthropicBuildOptions& opts);

// ── response normalisation ──────────────────────────────────────────────

struct NormalizedAnthropicResponse {
    std::optional<std::string> content;           // joined text parts
    nlohmann::json tool_calls = nullptr;          // OpenAI-style array or null
    std::optional<std::string> reasoning;         // joined thinking parts
    nlohmann::json reasoning_details = nullptr;   // per-block thinking blocks
    std::string finish_reason;                    // "stop"|"tool_calls"|"length"
    nlohmann::json usage = nullptr;               // raw usage block if present
};

/// Normalize the Anthropic messages.create() response body.  When
/// strip_tool_prefix is true, "mcp_" prefixes on tool_use names are
/// removed (mirror of the OAuth tool-name injection).
NormalizedAnthropicResponse normalize_anthropic_response(
    const nlohmann::json& response,
    bool strip_tool_prefix = false);

// ── OAuth ──────────────────────────────────────────────────────────────

/// True when the token format matches an Anthropic OAuth / setup token
/// (but NOT a regular sk-ant-api console key).
bool is_anthropic_oauth_token(std::string_view key);

/// Does the base_url require Bearer auth (OAuth) instead of x-api-key?
/// Native api.anthropic.com with an OAuth token → true.
bool requires_bearer_auth(std::string_view base_url,
                          std::string_view token);

/// Build the refresh_token POST body (either form-encoded or JSON) for
/// Anthropic's OAuth endpoint.  Returns the serialised body.
std::string build_oauth_refresh_body(std::string_view refresh_token,
                                     bool use_json);

/// Validate a claude-code credentials blob's shape and non-expiry.
/// Checks: has access_token, refresh_token; expires_at (if present)
/// is in the future with >=60s slack.
bool is_claude_code_token_valid(const nlohmann::json& creds);

struct PkcePair {
    std::string code_verifier;     // 43-128 char high-entropy string
    std::string code_challenge;    // URL-safe base64(sha256(verifier))
    std::string method;            // always "S256"
};

/// Generate a PKCE verifier/challenge pair.  The verifier is filled
/// deterministically from the supplied RNG seed for testability; when
/// seed == 0 a non-deterministic source is used.
PkcePair generate_pkce_pair(uint64_t rng_seed = 0);

// ── thinking block signature strategy ──────────────────────────────────

enum class ThinkingStrategy {
    StripAll,          // third-party endpoint
    KeepLatestOnly,    // native Anthropic
};

ThinkingStrategy thinking_strategy_for_base_url(std::string_view base_url);

// ── Anthropic-specific error classification ────────────────────────────

enum class AnthropicErrorKind {
    Transient,             // retry with backoff
    RateLimit,             // 429 — retry with longer backoff
    Overloaded,            // 529 — retry once after delay
    InvalidRequest,        // 400 — do not retry
    Authentication,        // 401 — do not retry (credential problem)
    PermissionDenied,      // 403 — do not retry
    NotFound,              // 404 — do not retry
    RequestTooLarge,       // 413 — do not retry (shrink input)
    InvalidSignature,      // 400 with "Invalid signature" — refresh thinking
    MaxTokensTooLarge,     // 400 with "max_tokens larger than…"
    ContextTooLong,        // 400 with context-length overflow hint
    ServerError,           // 500 — retry
    GatewayTimeout,        // 504 — retry
    Unknown,
};

/// Classify an error by status + body.  The body is inspected for
/// Anthropic-specific markers (signature, context_length, max_tokens).
AnthropicErrorKind classify_anthropic_error(int http_status,
                                            std::string_view body);

/// True when an AnthropicErrorKind should be retried (with provider-level
/// backoff).  Matches the retry taxonomy in auxiliary_client.py.
bool anthropic_error_is_retryable(AnthropicErrorKind kind);

/// Extract the available max_tokens value from an Anthropic 400 error body
/// like: "max_tokens: 65536 > 8192, which is the maximum allowed".
std::optional<int> parse_available_max_tokens(std::string_view body);

}  // namespace hermes::llm
