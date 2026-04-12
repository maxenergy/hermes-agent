// Factory for auxiliary LlmClient (used by title generator + context
// compressor — typically a cheap-and-fast model).
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <memory>

#include <nlohmann/json.hpp>

namespace hermes::agent {

// Build a concrete LlmClient from the `auxiliary` block in `config`:
//   auxiliary:
//     provider: "openai" | "anthropic" | "openrouter"
//     model: "gpt-4o-mini"
//     base_url: "https://..."   # optional
//     api_key: "${OPENAI_API_KEY}"  # already-expanded
//
// `transport` is non-owning and is shared across all created clients.
//
// If the auxiliary block is missing or has no provider, an unconfigured
// client is returned that throws std::runtime_error on any complete()
// call. This lets call sites unconditionally hold a non-null pointer.
std::unique_ptr<hermes::llm::LlmClient> make_auxiliary_client(
    const nlohmann::json& config,
    hermes::llm::HttpTransport* transport);

// Convenience: returns the configured auxiliary model name (empty if
// nothing was configured).
std::string get_auxiliary_model(const nlohmann::json& config);

}  // namespace hermes::agent
