// Model ID normalization and codex-model detection.
#pragma once

#include <string>
#include <string_view>

namespace hermes::llm {

// Strip all provider prefixes from a model ID:
//   "anthropic/claude-opus-4-6"             -> "claude-opus-4-6"
//   "openrouter/anthropic/claude-opus-4-6"  -> "claude-opus-4-6"
//   "claude-opus-4-6"                       -> "claude-opus-4-6"
std::string normalize_model_id(const std::string& model);

// Returns true for models compatible with code-generation tasks.
bool is_codex_model(const std::string& model);

}  // namespace hermes::llm
