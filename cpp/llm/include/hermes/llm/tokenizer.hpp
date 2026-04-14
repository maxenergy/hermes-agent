// Family-aware tokenizer and model registry.
//
// Port of the tokenizer-selection + pricing-registry parts of
// agent/model_metadata.py.  Provides:
//
//   * family_of(model)           — "claude" | "gpt" | "gemini" | "llama" | ...
//   * estimate_tokens(text, fam) — per-family chars/token heuristic
//   * count_tokens_messages(...) — full message-sequence estimate with
//                                  per-message role overhead
//   * lookup_model_info(model)   — richer ModelInfo with pricing + context +
//                                  capability flags from an expanded table
//
// The estimator is deterministic and does NOT call out to OpenAI's tiktoken
// or Anthropic's SDK — it's a heuristic sized against the Python version's
// character-ratio formulas.  Accurate enough for context-window accounting
// and cost previews; real byte-pair counts happen on the wire.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/llm/usage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

enum class ModelFamily {
    Unknown,
    Claude,     // Anthropic Claude (all generations)
    Gpt,        // OpenAI GPT family (3.5, 4, 4o, 4.1, 5, ...)
    Gemini,     // Google Gemini
    Llama,      // Meta Llama + derivatives
    Mistral,    // Mistral family
    Qwen,       // Alibaba Qwen / DashScope
    Deepseek,   // DeepSeek
    Glm,        // Zhipu GLM / z.ai
    Kimi,       // Moonshot Kimi
    Minimax,    // MiniMax
    Grok,       // xAI Grok
    Phi,        // Microsoft Phi
    Command,    // Cohere Command family
    Yi,         // 01.AI Yi
};

std::string_view family_name(ModelFamily f);

/// Classify a model slug.  Strips provider prefixes before matching.
ModelFamily family_of(std::string_view model);

/// Per-family characters-per-token heuristic.
///   Claude: ~3.5 chars/token
///   GPT:    ~4.0 chars/token
///   Gemini: ~4.2 chars/token
///   Llama:  ~3.8 chars/token
///   Qwen:   ~2.8 chars/token (CJK-heavy tokenizer)
///   Others: 4.0 (generic fallback)
double chars_per_token(ModelFamily f);

/// Estimate token count for a piece of text using the per-family ratio.
int64_t estimate_tokens(std::string_view text, ModelFamily family);

/// Estimate total tokens for an entire message sequence, accounting for
/// role/tool/tool_call overhead the way OpenAI's cookbook does (~4 tokens
/// per message + 2 tokens for the assistant reply priming).  Returns a
/// lower bound — always use provider-reported usage when available.
int64_t count_tokens_messages(const std::vector<Message>& messages,
                              ModelFamily family);

// ── richer model registry ───────────────────────────────────────────────

struct ModelInfo {
    std::string model_id;
    ModelFamily family = ModelFamily::Unknown;
    int64_t context_length = 0;       // total window (input + output)
    int64_t max_output_tokens = 0;    // native output cap
    bool supports_reasoning = false;
    bool supports_vision = false;
    bool supports_tools = true;
    bool supports_prompt_cache = false;
    bool supports_streaming = true;
    bool supports_json_mode = false;
    bool supports_parallel_tools = true;
    PricingTier pricing;
    std::string canonical_source = "hardcoded";
};

/// Lookup the richest known info for a model.  Falls back to family-level
/// defaults, then to a zero-initialized ModelInfo.
ModelInfo lookup_model_info(std::string_view model);

/// Return the list of models the registry knows about (diagnostics).
std::vector<std::string> list_known_models();

// ── prompt-budget planning ──────────────────────────────────────────────

struct BudgetPlan {
    int64_t input_budget = 0;         // max tokens we can send
    int64_t output_budget = 0;        // max_tokens to request
    int64_t safety_margin = 0;        // reserved headroom
    bool clamped = false;             // true when context_length forced clamp
};

/// Compute a budget plan for a given model: input_budget = context_length
/// − desired_output − safety_margin.  Falls back to 128k/16k defaults for
/// unknown models.  Used by the compressor and by routing decisions.
BudgetPlan plan_budget(std::string_view model,
                       int64_t desired_output_tokens,
                       int64_t safety_margin = 1024);

}  // namespace hermes::llm
