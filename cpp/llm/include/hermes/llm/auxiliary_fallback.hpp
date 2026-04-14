// Provider fallback chain for auxiliary / side-task LLM calls.
//
// Port of the resolution chain in agent/auxiliary_client.py:
//
//   1. Selected provider (or "auto")
//   2. OpenRouter
//   3. Nous Portal
//   4. Custom endpoint
//   5. Codex OAuth
//   6. Native Anthropic
//   7. Direct API-key providers (zai/kimi/minimax/...)
//
// The fallback activates on:
//   * HTTP 402 (Payment Required)
//   * HTTP 429 with "credits" / "insufficient funds" / "afford" in the body
//   * Connection errors on the primary provider
//
// Each attempt records a latency / cost / error observation so the caller
// can surface per-provider metrics.  The chain stops at the first
// successful provider OR when the chain is exhausted.
//
// This is a *planning* layer — actually dispatching a request to a
// provider is still the caller's job (each provider has its own
// LlmClient implementation).  AuxiliaryFallbackChain decides *what* to
// try next and tracks *how much* each attempt cost.
#pragma once

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/usage.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::llm {

/// Well-known auxiliary provider ids.  Mirrors the strings used by
/// auxiliary_client._resolve_provider_chain.
enum class AuxiliaryProvider {
    OpenRouter,
    NousPortal,
    Custom,
    CodexOauth,
    Anthropic,
    Gemini,
    Zai,
    KimiCoding,
    Minimax,
    MinimaxCn,
    AiGateway,
    OpencodeZen,
    OpencodeGo,
    Kilocode,
    Unknown,
};

std::string_view auxiliary_provider_name(AuxiliaryProvider p);
AuxiliaryProvider auxiliary_provider_from_name(std::string_view name);

/// Per-attempt observation captured by the chain.
struct AttemptObservation {
    AuxiliaryProvider provider = AuxiliaryProvider::Unknown;
    std::string model;
    std::chrono::milliseconds latency{0};
    CanonicalUsage usage;
    double estimated_cost_usd = 0.0;
    bool succeeded = false;
    std::optional<int> http_status;
    std::string error_message;
    std::string error_kind;  // "credits" | "rate_limit" | "connection" | "other"
};

/// Classify an error body to decide whether to advance the fallback chain.
/// Returns true when the error is either HTTP 402 or a credit / quota /
/// "afford" style message.  429 counts only when the body contains the
/// credit keywords — plain rate-limit 429s should be retried in-place.
bool is_credit_exhaustion_error(int http_status, std::string_view error_body);

/// True for transport-layer failures (connect/reset/timeout) that warrant
/// moving on to the next provider.
bool is_connection_error(std::string_view error_message);

/// Given a normalized provider id and a "for_vision" flag, return the
/// default small/fast auxiliary model for that provider.  Unknown
/// providers return an empty string.
std::string default_auxiliary_model(AuxiliaryProvider provider,
                                    bool for_vision = false);

/// Build the default fallback chain for a main provider.  The main
/// provider is placed first; remaining providers follow the auto chain
/// order.  Unknown / Main duplicates are filtered.
std::vector<AuxiliaryProvider> build_fallback_chain(
    AuxiliaryProvider main_provider,
    bool for_vision = false);

/// Planner — walks a fallback chain, invoking a caller-supplied dispatcher
/// per provider.  The dispatcher returns a completed AttemptObservation.
/// The walker stops at the first `succeeded=true` observation or when the
/// chain is exhausted.  All observations are appended to `history`.
struct FallbackResult {
    std::optional<AttemptObservation> winning_attempt;
    std::vector<AttemptObservation> history;
    double total_cost_usd = 0.0;
    std::chrono::milliseconds total_latency{0};
};

using AttemptDispatcher = std::function<AttemptObservation(
    AuxiliaryProvider provider,
    std::string_view model)>;

FallbackResult run_fallback_chain(
    const std::vector<AuxiliaryProvider>& chain,
    const AttemptDispatcher& dispatch,
    bool for_vision = false);

}  // namespace hermes::llm
