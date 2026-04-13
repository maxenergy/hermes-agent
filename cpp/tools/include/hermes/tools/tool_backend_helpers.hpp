// Phase 12: Helpers for tool backend selection (direct vs managed gateway).
//
// Mirrors the Python `tools/tool_backend_helpers.py` semantics — picks a
// backend based on environment flags + config.  Pure functions; no state.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace hermes::tools::backend_helpers {

/// Return true when the hidden Nous-managed tools feature flag is set in
/// the environment (``HERMES_ENABLE_NOUS_MANAGED_TOOLS=1``).
bool managed_nous_tools_enabled();

/// Normalise a browser cloud provider string — lowercases, strips whitespace,
/// and returns ``"local"`` when empty.
std::string normalize_browser_cloud_provider(const std::string& value);

/// Normalise / coerce a Modal mode to ``"auto"`` / ``"direct"`` / ``"managed"``
/// (defaults to ``"auto"``).
std::string coerce_modal_mode(const std::string& value);

/// Return true when direct Modal credentials are present (MODAL_TOKEN_ID +
/// MODAL_TOKEN_SECRET) or ``~/.modal.toml`` exists.
bool has_direct_modal_credentials();

struct ModalBackendState {
    std::string requested_mode;  // as user provided, after normalisation
    std::string mode;            // same as requested_mode here
    bool has_direct = false;
    bool managed_ready = false;
    bool managed_mode_blocked = false;
    std::optional<std::string> selected_backend;  // "managed"|"direct"|nullopt
};

/// Resolve the Modal backend selection given mode + availability flags.
ModalBackendState resolve_modal_backend_state(const std::string& modal_mode,
                                              bool has_direct,
                                              bool managed_ready);

/// Pick an HTTP backend tag for a named tool based on config.
/// Returns one of ``"default"`` / ``"managed_gateway"`` / ``"mock"``.
std::string select_backend(const std::string& tool_name,
                           const nlohmann::json& config);

/// Resolve the managed-gateway endpoint for a vendor (``firecrawl``, ``exa``,
/// ``brave``, ...).  Returns nullopt when no mapping is configured.
std::optional<std::string> resolve_vendor_endpoint(const std::string& vendor,
                                                   const nlohmann::json& config);

/// Apply backend-specific headers in-place (adds ``X-Nous-*`` for the
/// managed gateway, passes through for default).
void apply_backend_headers(
    std::unordered_map<std::string, std::string>& headers,
    const std::string& backend,
    const nlohmann::json& config);

/// Best-effort OpenAI audio key — prefers ``VOICE_TOOLS_OPENAI_KEY`` then
/// ``OPENAI_API_KEY``.  Returns an empty string when neither is set.
std::string resolve_openai_audio_api_key();

}  // namespace hermes::tools::backend_helpers
