// Runtime provider resolution — model string + config → concrete
// provider credentials.
//
// Ports the shape of `hermes_cli/runtime_provider.py`'s
// `resolve_runtime_provider()`.  The Python version is deeply
// entangled with Python-only concerns (httpx, tomllib, OAuth device
// flow, Nous portal refresh).  The C++17 port keeps only the parts
// that the synchronous LlmClient code path needs:
//
//   1. Look at the model string and config to pick a provider name.
//   2. Consult CredentialPool for that provider.
//   3. Fill in the default base_url for the provider.
//
// OAuth refresh, portal agent-key minting, and per-provider token
// refresh live in cpp/auth/.  The production wiring plugs those
// modules in as CredentialPool refreshers at startup.
#pragma once

#include "hermes/llm/credential_pool.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hermes::llm {

struct ResolvedProvider {
    std::string provider_name;   // "anthropic", "openai", "qwen-oauth", ...
    std::string api_key;         // bearer token or API key
    std::string base_url;        // inference endpoint
    std::string model_id;        // normalized model ID to send to the provider
    std::string api_mode;        // "chat_completions" | "anthropic_messages" | "codex_responses"
    std::string source;          // "pool", "config", "env", "default", ...
};

// Pick a provider slug from a model name alone.  Used as the fallback
// when config doesn't specify `model.provider`.
//
//   "claude-sonnet-4-6"      → "anthropic"
//   "gpt-4o"                 → "openai"
//   "gpt-5.3-codex"          → "openai-codex"
//   "qwen3-coder-plus"       → "qwen-oauth"
//   "anthropic/claude-opus"  → "anthropic"
//   "openrouter/..."         → "openrouter"
std::string infer_provider_from_model(const std::string& model);

// Default inference base URL for a known provider slug.  Returns an
// empty string for unknown providers (caller must supply base_url
// via config).
std::string default_base_url_for_provider(const std::string& provider);

// Default api_mode for a provider.
std::string default_api_mode_for_provider(const std::string& provider);

// Resolve a full runtime provider from a model name + config tree.
//
// Precedence (mirrors Python):
//   1. config["model"]["provider"] (if explicitly set, non-"auto")
//   2. infer_provider_from_model(model)
//
// For api_key/base_url:
//   1. config["model"]["api_key"] / config["model"]["base_url"] (explicit)
//   2. CredentialPool entry for the provider (possibly via refresher)
//   3. `HERMES_<PROVIDER>_API_KEY` / `<PROVIDER>_API_KEY` env var
//   4. Default base_url for the provider
//
// Throws std::runtime_error when no usable credentials can be resolved
// (and the caller hasn't passed allow_missing_key=true).
ResolvedProvider resolve_runtime_provider(
    const std::string& model,
    const nlohmann::json& config,
    CredentialPool* pool = nullptr,
    bool allow_missing_key = false);

}  // namespace hermes::llm
