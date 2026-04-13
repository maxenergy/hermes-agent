// Hot-swap the active model without tearing down the session.
//
// Ports the gist of `hermes_cli/model_switch.py`:  given a new model
// name, pick a provider, build a fresh LlmClient, and hand back a
// ModelSwitchResult that describes what changed.  The caller (CLI
// `/model` slash command or gateway handler) is responsible for
// plumbing the new client into the active AIAgent.
//
// "Session state preservation" here means:  the tier-down fallback
// list is preserved across switches, and the model-switch API never
// mutates the message history — only the client + metadata.  Tokenizer
// invalidation is a hook for the ContextCompressor integration (when
// that lands in cpp/agent/) — the result carries a flag that tells
// callers whether they need to recompute token counts.
#pragma once

#include "hermes/llm/credential_pool.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/runtime_provider.hpp"

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hermes::llm {

struct ModelSwitchResult {
    // The fully-resolved provider descriptor produced by
    // resolve_runtime_provider().
    ResolvedProvider resolved;
    // New LlmClient instance bound to the resolved provider.  Owned by
    // the caller; swap it into the active AIAgent atomically.
    std::shared_ptr<LlmClient> client;
    // True when the tokenizer family changed (e.g. OpenAI → Anthropic)
    // and callers should invalidate any cached token counts.
    bool tokenizer_invalidated = false;
    // A human-readable summary, suitable for printing in `/model`
    // output.  Includes provider, model, base URL, and source.
    std::string summary;
    // Fallback tier-down list, in preference order (cheaper / shorter
    // context models first fallback).  May be empty.
    std::vector<std::string> tier_down;
};

// Factory used to build LlmClients after provider resolution.  The
// production version wires in OpenAIClient / AnthropicClient /
// OpenRouterClient based on api_mode; tests inject a stub.
using LlmClientFactory = std::function<std::shared_ptr<LlmClient>(
    const ResolvedProvider&)>;

// Register a custom factory.  Pass `nullptr` to reset to the default
// (which currently throws — cpp/agent wires the real one).  Thread-safe.
void set_llm_client_factory(LlmClientFactory factory);

// Access the singleton LlmClientFactory (may be default/empty).
LlmClientFactory get_llm_client_factory();

// The core API — swap to `new_model`.  Consults the current config
// (caller-supplied), resolves the runtime provider, builds a new
// client, and returns both.  Does NOT touch any global AIAgent state —
// the caller is the single source of truth for "active model".
//
// `previous_provider` is optional; when supplied, it's used to decide
// whether the tokenizer family changed.
ModelSwitchResult switch_model(
    const std::string& new_model,
    const nlohmann::json& config,
    const std::string& previous_provider = "",
    CredentialPool* pool = nullptr);

// Build the tier-down fallback list for a given model.  Returns a list
// of models to try when the primary exceeds context or hits rate limits.
// Mirrors the shape of `agent/smart_routing.py:tier_down_for_context`
// but operates purely on model names (no context-length math here —
// that's in smart_routing.cpp).
std::vector<std::string> build_tier_down(const std::string& model);

}  // namespace hermes::llm
