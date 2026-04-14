// Anthropic prompt caching (system_and_3 strategy).
//
// C++17 port of agent/prompt_caching.py. Places up to 4 cache_control
// breakpoints on the message list: the system prompt plus the last 3
// non-system messages. Reduces input token costs ~75% on multi-turn
// conversations.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hermes::agent {

// Apply the system_and_3 caching strategy. Returns a deep-copied message
// list with cache_control breakpoints injected.
//
// cache_ttl: either "5m" (default Anthropic ephemeral) or "1h".
// native_anthropic: true when emitting to Anthropic's native API format —
//     tool messages carry cache_control at the top level; OpenAI-flavored
//     adapters skip tool-role messages.
nlohmann::json apply_anthropic_cache_control(
    const nlohmann::json& api_messages,
    const std::string& cache_ttl = "5m",
    bool native_anthropic = false);

}  // namespace hermes::agent
