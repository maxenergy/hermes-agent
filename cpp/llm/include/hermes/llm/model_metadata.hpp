// Model metadata (context length / pricing / capabilities) with token
// estimation and error-body context-limit parsing.  Port of subset of
// agent/model_metadata.py.
#pragma once

#include "hermes/llm/message.hpp"
#include "hermes/llm/usage.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

struct ModelMetadata {
    std::string model_id;
    std::string family;
    int64_t context_length = 0;  // -1 if unknown
    bool supports_reasoning = false;
    bool supports_vision = false;
    bool supports_prompt_cache = false;
    PricingTier pricing;
    std::string source;  // "hardcoded" | "models_dev" | "probed"
};

// Synchronous lookup.  Phase 3: hardcoded table fallback; base_url is
// accepted but unused (reserved for Phase 4 live probing).
ModelMetadata fetch_model_metadata(std::string_view model,
                                   std::string_view base_url = "");

// ~4 chars per token heuristic.  Returns 0 for empty input.
int64_t estimate_tokens_rough(std::string_view text);
int64_t estimate_messages_tokens_rough(const std::vector<Message>& messages);

// Scan an error body for phrases like "maximum context length is 32768" and
// return the parsed integer if it falls in a sane range.
std::optional<int64_t> parse_context_limit_from_error(std::string_view error_body);

// Descending probe tiers used by the runtime loop when the true context
// length is unknown.
constexpr std::array<int64_t, 5> CONTEXT_PROBE_TIERS = {
    128000, 64000, 32000, 16000, 8000,
};

// "anthropic/claude-opus-4-6" → "claude-opus-4-6".  Leaves Ollama-style
// "llama3:70b" alone.
std::string strip_provider_prefix(std::string_view model);

// Query a local Ollama instance for the model's num_ctx parameter.
// Returns std::nullopt on any failure (Ollama not running, model not
// found, etc.).
std::optional<int64_t> query_ollama_num_ctx(const std::string& model);

}  // namespace hermes::llm
