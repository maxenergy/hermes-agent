// Deep model metadata — context windows, pricing, family detection,
// provider inference, tokenizer mapping, completion budget planner.
//
// Port of agent/model_metadata.py.  The hardcoded defaults in this header
// are the "thin fallback" used when models.dev / OpenRouter / provider
// probing all miss.  This layer is pure (no network); live fetches live
// in models_dev.cpp and anthropic_client.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

// ── context probe tiers ─────────────────────────────────────────────────

constexpr std::array<int64_t, 5> kContextProbeTiers = {
    128000, 64000, 32000, 16000, 8000};
constexpr int64_t kDefaultFallbackContext = 128000;

/// Next-lower probe tier, or nullopt when already at the minimum.
std::optional<int64_t> get_next_probe_tier(int64_t current);

// ── provider prefix stripping ───────────────────────────────────────────

/// True when the string (case-insensitively) is a known provider prefix
/// that should be stripped from a model name like "openrouter:claude-*".
bool is_known_provider_prefix(std::string_view prefix);

/// Strip a recognised provider prefix.  Preserves Ollama-style "model:tag"
/// forms (e.g. "llama3:70b") by matching the suffix against a tag regex.
std::string strip_model_provider_prefix(std::string_view model);

// ── URL-to-provider inference ───────────────────────────────────────────

/// Infer the canonical provider name (as used by models.dev) from a
/// base_url.  Recognises api.openai.com, api.anthropic.com, api.z.ai,
/// api.moonshot.ai, dashscope.aliyuncs.com, openrouter.ai, xai, etc.
/// Returns empty when the host is not recognised.
std::string infer_provider_from_base_url(std::string_view base_url);

/// True when the base_url matches a recognised provider host (i.e.
/// infer_provider_from_base_url returned non-empty).
bool is_known_provider_base_url(std::string_view base_url);

// ── local endpoint detection ───────────────────────────────────────────

/// True when the base_url's host resolves to a local / private address:
/// localhost, 127.0.0.0/8, ::1, RFC-1918 (10/8, 172.16/12, 192.168/16).
/// Pure string check — no DNS lookup.
bool is_local_endpoint(std::string_view base_url);

enum class LocalServerType {
    Unknown,
    Ollama,
    LmStudio,
    Vllm,
    LlamaCpp,
};

std::string_view local_server_type_name(LocalServerType t);

// ── hardcoded context-length table ─────────────────────────────────────

struct ContextEntry {
    std::string_view key;     // substring match (longest-prefix wins)
    int64_t context;
};

/// Broad family fallback for context lengths.  Sorted roughly by specificity
/// (longer keys before shorter catchalls).  Keys use substring match with
/// longest-prefix-wins semantics.
const std::vector<ContextEntry>& default_context_table();

/// Look up the model's context length from the hardcoded table.  Returns
/// kDefaultFallbackContext when no entry matches.
int64_t lookup_default_context_length(std::string_view model);

// ── model family detection ─────────────────────────────────────────────

enum class ModelFamily {
    Unknown,
    ClaudeOpus,
    ClaudeSonnet,
    ClaudeHaiku,
    Gpt4,
    Gpt4o,
    Gpt5,
    O1,
    O3,
    Gemini,
    Gemma,
    DeepSeek,
    Qwen,
    QwenCoder,
    Llama,
    Mistral,
    Minimax,
    Glm,
    Kimi,
    Grok,
    Hermes,
    Codex,
};

std::string_view model_family_name(ModelFamily f);
ModelFamily detect_model_family(std::string_view model);

// ── extended pricing table ─────────────────────────────────────────────

struct ExtendedPricing {
    double prompt_per_mtok = 0;          // $/M input tokens
    double completion_per_mtok = 0;      // $/M output tokens
    double cache_read_per_mtok = 0;
    double cache_write_per_mtok = 0;
    double cache_1h_write_per_mtok = 0;  // 1h extended cache
    bool has_1h_cache = false;
    int64_t context_length = 0;
    int64_t max_output = 0;
    ModelFamily family = ModelFamily::Unknown;
    bool supports_vision = false;
    bool supports_tools = true;
    bool supports_prompt_cache = false;
    bool supports_extended_cache_1h = false;
};

/// Comprehensive pricing/context/caps lookup for a model id.  Matches on
/// longest-substring from an internal table of 100+ model entries.
ExtendedPricing lookup_extended_pricing(std::string_view model);

// ── tokenizer family mapping ───────────────────────────────────────────

enum class TokenizerFamily {
    Unknown,
    Cl100k,          // GPT-4, GPT-4o
    O200k,           // GPT-4o, o1, o3
    Claude,          // Claude tokenizer
    Gemini,          // Gemini SentencePiece
    Qwen,            // Qwen BPE
    Llama,           // LLaMA sentencepiece
    Mistral,
    DeepSeek,
    CharHeuristic,   // fall back to ~4 chars per token
};

TokenizerFamily tokenizer_family_for_model(std::string_view model);
std::string_view tokenizer_family_name(TokenizerFamily t);

// ── completion budget planner ──────────────────────────────────────────

struct BudgetPlan {
    int64_t prompt_tokens = 0;       // estimated
    int64_t reserved_output = 0;     // what we'll cap the model output at
    int64_t available_context = 0;   // context_length - prompt_tokens
    bool overflow = false;           // prompt won't even fit
};

/// Plan the output token budget given an estimated prompt size and a
/// target model.  The planner uses the model's native output ceiling as
/// an upper bound, then reserves min(ceiling, available_context - slack)
/// for output.  Slack is a small (256 tokens) safety margin.
BudgetPlan plan_completion_budget(std::string_view model,
                                  int64_t estimated_prompt_tokens,
                                  int64_t context_length = 0);

// ── context cache persistence shape ────────────────────────────────────

struct ContextCacheKey {
    std::string model;
    std::string base_url;
};

/// Build the cache key string used for persistent context-length caching
/// ("model@base_url").  Empty base_url is allowed.
std::string make_context_cache_key(std::string_view model,
                                   std::string_view base_url);

/// Parse "model@base_url" back into its components.  Returns nullopt on
/// malformed input.
std::optional<ContextCacheKey> parse_context_cache_key(std::string_view key);

// ── pricing utilities ──────────────────────────────────────────────────

/// Compute the USD cost for a call with the given input/output/cache usage.
/// All inputs are absolute token counts.  Returns 0 when pricing is unset.
double compute_call_cost_usd(const ExtendedPricing& p,
                             int64_t input_tokens,
                             int64_t output_tokens,
                             int64_t cache_read_tokens = 0,
                             int64_t cache_write_tokens = 0,
                             int64_t cache_1h_write_tokens = 0);

/// Estimate tokens from a character count using the model's tokenizer
/// family.  Hermes-internal estimates; not a perfect tokeniser.
int64_t estimate_tokens_from_chars(int64_t chars, TokenizerFamily family);

}  // namespace hermes::llm
