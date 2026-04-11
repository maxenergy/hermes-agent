// Anthropic prompt-cache injection.  Port of agent/prompt_caching.py.
//
// Strategy: "system_and_3" — place cache_control markers on
//   1. the system message (if present)
//   2/3/4. the last three non-system messages
// Max 4 breakpoints total (Anthropic hard limit).
#pragma once

#include "hermes/llm/message.hpp"

#include <string>
#include <vector>

namespace hermes::llm {

struct PromptCacheOptions {
    std::string cache_ttl = "5m";       // "5m" or "1h"
    bool native_anthropic = false;      // only inject when true
};

// CRITICAL invariants:
//   * Never mutates past context beyond adding cache_control markers.
//   * Max 4 breakpoints total (Anthropic hard limit).
//   * Idempotent: calling twice produces the same result.
//   * Operates in place — does not deep-copy.
//
// When native_anthropic == false, this is a no-op (OpenRouter etc.  don't
// support Anthropic's cache_control field).
void apply_anthropic_cache_control(std::vector<Message>& messages,
                                   const PromptCacheOptions& opts = {});

}  // namespace hermes::llm
