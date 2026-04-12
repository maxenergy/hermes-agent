// Smart model routing — fallback chains and context-overflow tier-down.
#pragma once

#include "hermes/llm/error_classifier.hpp"

#include <optional>
#include <string>
#include <vector>

namespace hermes::llm {

struct FallbackChain {
    std::string primary;
    std::vector<std::string> fallbacks;
};

// Given a failed model + reason, suggest the next model to try.
std::optional<std::string> suggest_fallback(const std::string& model,
                                             FailoverReason reason);

// On context overflow, suggest a model with larger context window.
std::optional<std::string> tier_down_for_context(const std::string& model);

}  // namespace hermes::llm
