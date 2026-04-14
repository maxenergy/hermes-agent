// Anthropic request/response feature helpers.
//
// Split out from anthropic_client.cpp so each helper can be unit-tested
// in isolation.  Covers the request-shaping features Python's
// agent/anthropic_adapter.py applies before calling messages.create():
//
//   * tool_choice mapping (OpenAI "auto|required|none|<name>" → Anthropic
//     {"type":"auto|any|tool","name":...})
//   * thinking config (manual budget_tokens / adaptive output_config.effort)
//   * stop_sequences / top_p / top_k / service_tier plumbing
//   * beta header selection per base_url
//   * Claude-Code OAuth sanitization (system prefix + tool_name prefix)
//   * stop_reason → finish_reason mapping
//   * reasoning block extraction (thinking + redacted_thinking)
//
// All helpers are pure (no I/O) so they stay side-effect free and easy to
// fuzz.
#pragma once

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

// ── tool_choice ─────────────────────────────────────────────────────────

/// Map an OpenAI-flavoured tool_choice string to the Anthropic wire shape.
/// Returns std::nullopt when the caller asked for "none" (the canonical
/// Anthropic response is to drop tools entirely — the caller enforces that).
///
///   "auto"     → {"type":"auto"}
///   nullptr    → {"type":"auto"}           (default)
///   "required" → {"type":"any"}
///   "none"     → std::nullopt             (caller drops tools)
///   "<name>"   → {"type":"tool","name":"<name>"}
std::optional<nlohmann::json> map_tool_choice_to_anthropic(
    std::string_view openai_choice);

// ── thinking / reasoning ────────────────────────────────────────────────

/// Effort level to budget-tokens lookup.  Mirrors THINKING_BUDGET in
/// anthropic_adapter.py.
///   "minimal"|"low"     → 2048
///   "medium" (default)  → 8000
///   "high"              → 16000
///   "maximum"           → 32000
int thinking_budget_for_effort(std::string_view effort);

/// Map "minimal|low|medium|high|maximum" to the Anthropic
/// ``output_config.effort`` enum used by adaptive thinking.
///   "minimal" → "minimal"
///   "low"     → "low"
///   "medium"  → "medium"
///   "high"    → "high"
///   "maximum" → "high"   (Anthropic caps at high)
std::string_view map_adaptive_effort(std::string_view effort);

/// True when the model supports adaptive thinking (Claude 4.6 family).
bool supports_adaptive_thinking(std::string_view model);

/// True when the model supports extended thinking at all (Haiku does not).
bool supports_extended_thinking(std::string_view model);

/// Build the thinking request fragment and optionally adjust temperature /
/// max_tokens for manual-budget thinking.  Returns a JSON object with
/// {"thinking": ..., "output_config": ..., "temperature": 1,
///  "max_tokens": ...} keys to merge into the request body.  When the model
/// doesn't support thinking an empty object is returned.
nlohmann::json build_thinking_config(std::string_view model,
                                     std::string_view effort,
                                     int current_max_tokens);

// ── stop_reason mapping ─────────────────────────────────────────────────

/// "end_turn"|"tool_use"|"max_tokens"|"stop_sequence"|"pause_turn" →
/// OpenAI-style finish_reason.
std::string map_anthropic_stop_reason(std::string_view stop_reason);

// ── stop_sequences / sampling helpers ───────────────────────────────────

struct AnthropicRequestExtras {
    std::vector<std::string> stop_sequences;
    std::optional<double> top_p;
    std::optional<int> top_k;
    std::optional<std::string> service_tier;  // "auto"|"standard_only"|"priority"
    std::optional<std::string> tool_choice;   // raw OpenAI string
    std::optional<std::string> thinking_effort;
    bool fast_mode = false;
    bool is_oauth = false;
};

/// Extract AnthropicRequestExtras from the provider-agnostic req.extra
/// JSON blob.  Keys recognised:
///   "stop_sequences"   : [str,...]
///   "top_p"            : number
///   "top_k"            : int
///   "service_tier"     : str
///   "tool_choice"      : str
///   "thinking_effort"  : str
///   "fast_mode"        : bool
///   "is_oauth"         : bool
AnthropicRequestExtras parse_anthropic_extras(const nlohmann::json& extra);

// ── beta headers ────────────────────────────────────────────────────────

/// Default beta header list for the given base_url.  Mirrors
/// _common_betas_for_base_url — native endpoints get the full list;
/// third-party compatible endpoints get a trimmed list without
/// fine-grained-tool-streaming.
std::vector<std::string> common_betas_for_base_url(std::string_view base_url);

/// True when base_url points to a non-native Anthropic-compatible endpoint
/// (z.ai, Kimi, MiniMax, DashScope, OpenRouter, etc.).  Used to gate
/// beta headers and prompt-cache features.
bool is_third_party_anthropic_endpoint(std::string_view base_url);

// ── Claude Code OAuth sanitization ──────────────────────────────────────

/// Replace product-name references in a system prompt that would otherwise
/// trip Anthropic's server-side content filters when using a Claude Code
/// OAuth token.  Mirrors the inline sanitizer in build_anthropic_kwargs.
std::string sanitize_for_claude_code_oauth(std::string_view input);

/// Prefix tool names with "mcp_" per the Claude Code convention.  No-op
/// when the tool name is already prefixed.
std::string apply_mcp_tool_prefix(std::string_view name);

/// Strip the "mcp_" prefix, if present.
std::string strip_mcp_tool_prefix(std::string_view name);

// ── reasoning block extraction ──────────────────────────────────────────

struct ExtractedReasoning {
    std::string text;                     // joined thinking text
    std::vector<nlohmann::json> blocks;   // raw thinking/redacted_thinking blocks
    bool has_signature = false;           // any block had a "signature" field
};

/// Walk a parsed Anthropic response's content array and pull out
/// thinking / redacted_thinking blocks for preservation.
ExtractedReasoning extract_reasoning_blocks(const nlohmann::json& content_array);

// ── stop-sequences normalization ────────────────────────────────────────

/// Deduplicate, drop empties, and cap at 4 stop sequences (Anthropic max).
std::vector<std::string> normalize_stop_sequences(
    const std::vector<std::string>& raw);

// ── cache breakpoint inspector ──────────────────────────────────────────

struct CacheBreakpointInfo {
    int total_breakpoints = 0;
    int system_breakpoints = 0;
    int message_breakpoints = 0;
    std::vector<int> breakpoint_indices;  // message indices carrying a marker
};

/// Count cache_control breakpoints applied to a message vector.  Useful in
/// tests to verify the Anthropic 4-breakpoint hard limit.
CacheBreakpointInfo inspect_cache_breakpoints(
    const std::vector<Message>& messages);

}  // namespace hermes::llm
