// C++17 port of the pure-logic helpers from
// `hermes_cli/nous_subscription.py`.
//
// The full `get_nous_subscription_features` entry point threads through
// three different runtime singletons (`load_config`, Nous auth state,
// managed-gateway readiness probes).  This port focuses on the pieces
// that have no side effects and are therefore easy to test in
// isolation:
//
//   * `model_config_dict` -- coerces a free-form `"model"` config node
//     into a canonical dict, matching the Python helper.
//   * `browser_label` / `tts_label` -- user-facing provider names for
//     the status panel.
//   * `resolve_browser_feature_state` -- deterministic availability /
//     active / managed decision tree used by the status engine.
//   * `nous_feature_state` / `nous_subscription_features` -- data
//     classes that mirror `NousFeatureState` and
//     `NousSubscriptionFeatures`.
//   * `modal_provider_label` -- helper used when populating the modal
//     row.
//   * `nous_subscription_explainer_lines` -- the three-line blurb
//     displayed beneath the status panel when managed tools are
//     available.
//   * `apply_nous_managed_defaults_decision` -- decides which
//     toolset-level defaults should be applied given the inputs; the
//     caller owns the actual config mutation.
//
// The `NousFeatureState` struct omits the Python property accessors
// (`features.web`, `features.tts`, ...) because C++ callers access
// the map directly.
#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::cli::nous_subscription {

// Ordered list of feature keys matching
// `NousSubscriptionFeatures.items()`.
inline constexpr std::array<const char*, 5> k_feature_order{
    "web", "image_gen", "tts", "browser", "modal",
};

struct nous_feature_state {
    std::string key{};
    std::string label{};
    bool included_by_default{false};
    bool available{false};
    bool active{false};
    bool managed_by_nous{false};
    bool direct_override{false};
    bool toolset_enabled{false};
    std::string current_provider{};
    bool explicit_configured{false};
};

struct nous_subscription_features {
    bool subscribed{false};
    bool nous_auth_present{false};
    bool provider_is_nous{false};
    std::unordered_map<std::string, nous_feature_state> features{};

    // Helpers mirroring the Python `.web`, `.tts`, etc. properties.
    const nous_feature_state& at(const std::string& key) const;
    std::vector<nous_feature_state> ordered_items() const;
};

// Coerce `config["model"]` into a dict: accepts the raw object (kept
// as-is), a non-empty string (wrapped as `{"default": value}`), and
// anything else (returned as an empty object).
nlohmann::json model_config_dict(const nlohmann::json& config);

// User-facing labels.  Empty / unknown inputs fall back to the default
// provider label as the Python implementation does.
std::string browser_label(const std::string& current_provider);
std::string tts_label(const std::string& current_provider);

// Pure-logic result of `_resolve_browser_feature_state` -- computed
// from the same inputs as the Python function.
struct browser_feature_decision {
    std::string current_provider{};
    bool available{false};
    bool active{false};
    bool managed{false};
};

struct browser_feature_inputs {
    bool browser_tool_enabled{false};
    std::string browser_provider{};
    bool browser_provider_explicit{false};
    bool browser_local_available{false};
    bool direct_camofox{false};
    bool direct_browserbase{false};
    bool direct_browser_use{false};
    bool direct_firecrawl{false};
    bool managed_browser_available{false};
};

browser_feature_decision resolve_browser_feature_state(
    const browser_feature_inputs& inputs);

// Three-line blurb shown below the status panel when managed Nous
// tools are available.  Returns an empty list when `managed_enabled`
// is false (mirrors the Python gate on `managed_nous_tools_enabled()`).
std::vector<std::string> nous_subscription_explainer_lines(bool managed_enabled);

// Inputs for the small `apply_nous_managed_defaults` decision helper.
// The caller is responsible for mutating the underlying config when the
// helper returns a non-empty set.
struct managed_defaults_inputs {
    bool managed_enabled{false};
    bool provider_is_nous{false};
    std::set<std::string> selected_toolsets{};
    // Per-feature "explicit_configured" flags harvested from
    // `nous_subscription_features`.
    bool web_explicit_configured{false};
    bool tts_explicit_configured{false};
    bool browser_explicit_configured{false};
    // Env-var presence checks -- true if any of the listed keys is set
    // to a non-empty value.
    bool has_parallel_or_tavily_or_firecrawl_env{false};
    bool has_openai_audio_or_elevenlabs_env{false};
    bool has_browser_use_or_browserbase_env{false};
    bool has_fal_env{false};
};

struct managed_defaults_decision {
    std::set<std::string> to_change{};
    // When `true`, the caller should overwrite `config["web"]["backend"]`
    // with `"firecrawl"`.
    bool set_web_backend_firecrawl{false};
    // When `true`, overwrite `config["tts"]["provider"]` with `"openai"`.
    bool set_tts_provider_openai{false};
    // When `true`, overwrite `config["browser"]["cloud_provider"]` with
    // `"browser-use"`.
    bool set_browser_cloud_provider_browser_use{false};
};

managed_defaults_decision apply_nous_managed_defaults_decision(
    const managed_defaults_inputs& inputs);

// Decide the tts-provider override from `apply_nous_provider_defaults`.
// Returns the set of config keys that changed alongside the new value.
struct provider_defaults_decision {
    std::set<std::string> to_change{};
    bool set_tts_provider_openai{false};
};

provider_defaults_decision apply_nous_provider_defaults_decision(
    bool managed_enabled,
    bool provider_is_nous,
    const std::string& current_tts_provider);

}  // namespace hermes::cli::nous_subscription
