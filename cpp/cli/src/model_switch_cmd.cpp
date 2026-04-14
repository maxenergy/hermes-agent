// C++17 port of hermes_cli/model_switch.py.
#include "hermes/cli/model_switch_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace hermes::cli::model_switch_cmd {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) out.push_back(std::move(w));
    return out;
}

// Replace every occurrence of *needle* with the empty string.  Used to
// scrub "--global" out of a free-form args string.
std::string strip_token(std::string s, const std::string& needle) {
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.erase(pos, needle.size());
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Non-agentic model warning
// ---------------------------------------------------------------------------

const char* const HERMES_MODEL_WARNING =
    "Nous Research Hermes 3 & 4 models are NOT agentic and are not designed "
    "for use with Hermes Agent. They lack the tool-calling capabilities "
    "required for agent workflows. Consider using an agentic model instead "
    "(Claude, GPT, Gemini, DeepSeek, etc.).";

std::string check_hermes_model_warning(const std::string& model_name) {
    if (to_lower(model_name).find("hermes") != std::string::npos) {
        return HERMES_MODEL_WARNING;
    }
    return "";
}

// ---------------------------------------------------------------------------
// Alias tables
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, ModelIdentity>& model_aliases() {
    static const std::unordered_map<std::string, ModelIdentity> tbl = {
        // Anthropic
        {"sonnet",   {"anthropic",    "claude-sonnet"}},
        {"opus",     {"anthropic",    "claude-opus"}},
        {"haiku",    {"anthropic",    "claude-haiku"}},
        {"claude",   {"anthropic",    "claude"}},
        // OpenAI
        {"gpt5",     {"openai",       "gpt-5"}},
        {"gpt",      {"openai",       "gpt"}},
        {"codex",    {"openai",       "codex"}},
        {"o3",       {"openai",       "o3"}},
        {"o4",       {"openai",       "o4"}},
        // Google
        {"gemini",   {"google",       "gemini"}},
        // DeepSeek
        {"deepseek", {"deepseek",     "deepseek-chat"}},
        // xAI
        {"grok",     {"x-ai",         "grok"}},
        // Meta
        {"llama",    {"meta-llama",   "llama"}},
        // Qwen
        {"qwen",     {"qwen",         "qwen"}},
        // MiniMax
        {"minimax",  {"minimax",      "minimax"}},
        // Nvidia
        {"nemotron", {"nvidia",       "nemotron"}},
        // Kimi
        {"kimi",     {"moonshotai",   "kimi"}},
        // Z.AI / GLM
        {"glm",      {"z-ai",         "glm"}},
        // StepFun
        {"step",     {"stepfun",      "step"}},
        // Xiaomi
        {"mimo",     {"xiaomi",       "mimo"}},
        // Arcee
        {"trinity",  {"arcee-ai",     "trinity"}},
    };
    return tbl;
}

std::unordered_map<std::string, DirectAlias> load_direct_aliases(
    const nlohmann::json& config) {
    std::unordered_map<std::string, DirectAlias> merged;
    if (!config.is_object()) return merged;
    auto it = config.find("model_aliases");
    if (it == config.end() || !it->is_object()) return merged;

    for (auto alias_it = it->begin(); alias_it != it->end(); ++alias_it) {
        if (!alias_it.value().is_object()) continue;
        const auto& entry = alias_it.value();
        auto get_str = [&](const char* k, const char* fallback) -> std::string {
            auto v = entry.find(k);
            if (v != entry.end() && v->is_string()) return v->get<std::string>();
            return fallback ? fallback : "";
        };
        std::string model = get_str("model", "");
        if (model.empty()) continue;
        DirectAlias da;
        da.model = model;
        da.provider = get_str("provider", "custom");
        da.base_url = get_str("base_url", "");
        merged[to_lower(trim(alias_it.key()))] = std::move(da);
    }
    return merged;
}

// ---------------------------------------------------------------------------
// Flag parsing
// ---------------------------------------------------------------------------

ModelFlags parse_model_flags(const std::string& raw_args) {
    ModelFlags out;
    std::string s = raw_args;

    // Extract --global (textual match).
    if (s.find("--global") != std::string::npos) {
        out.is_global = true;
        s = trim(strip_token(s, "--global"));
    }

    // Split into tokens, extract --provider <name>.
    auto parts = split_ws(s);
    std::vector<std::string> filtered;
    filtered.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "--provider" && i + 1 < parts.size()) {
            out.explicit_provider = parts[i + 1];
            ++i;
            continue;
        }
        filtered.push_back(parts[i]);
    }

    // Re-join the remaining tokens — preserves model names with spaces.
    std::ostringstream oss;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (i) oss << ' ';
        oss << filtered[i];
    }
    out.model_input = trim(oss.str());
    return out;
}

// ---------------------------------------------------------------------------
// Alias resolution
// ---------------------------------------------------------------------------

std::optional<AliasResolution> resolve_alias(
    const std::string& raw_input,
    const std::string& current_provider,
    const std::vector<std::string>& catalog,
    const std::unordered_map<std::string, DirectAlias>& direct_aliases) {
    std::string key = to_lower(trim(raw_input));
    if (key.empty()) return std::nullopt;

    // Direct alias (exact model+provider+base_url) — highest priority.
    auto da_it = direct_aliases.find(key);
    if (da_it != direct_aliases.end()) {
        AliasResolution r;
        r.provider = da_it->second.provider;
        r.model = da_it->second.model;
        r.alias_name = key;
        return r;
    }
    // Reverse lookup: match by full model name.
    for (const auto& kv : direct_aliases) {
        if (to_lower(kv.second.model) == key) {
            AliasResolution r;
            r.provider = kv.second.provider;
            r.model = kv.second.model;
            r.alias_name = kv.first;
            return r;
        }
    }

    // Catalog resolution via short-name alias.
    auto mit = model_aliases().find(key);
    if (mit == model_aliases().end()) return std::nullopt;
    const auto& identity = mit->second;

    if (catalog.empty()) return std::nullopt;

    bool aggregator = providers_cmd::is_aggregator(current_provider);
    std::string prefix = aggregator
        ? to_lower(identity.vendor + "/" + identity.family)
        : to_lower(identity.family);

    for (const auto& mid : catalog) {
        std::string lower = to_lower(mid);
        if (starts_with(lower, prefix)) {
            AliasResolution r;
            r.provider = current_provider;
            r.model = mid;
            r.alias_name = key;
            return r;
        }
    }
    return std::nullopt;
}

std::optional<AliasResolution> resolve_alias_fallback(
    const std::string& raw_input,
    const std::vector<std::string>& authenticated_providers,
    const std::unordered_map<std::string, std::vector<std::string>>& catalogs_by_provider,
    const std::unordered_map<std::string, DirectAlias>& direct_aliases) {
    std::vector<std::string> providers = authenticated_providers;
    if (providers.empty()) providers = {"openrouter", "nous"};
    for (const auto& prov : providers) {
        auto cat_it = catalogs_by_provider.find(prov);
        const std::vector<std::string>& cat =
            (cat_it != catalogs_by_provider.end()) ? cat_it->second
                                                   : std::vector<std::string>{};
        auto r = resolve_alias(raw_input, prov, cat, direct_aliases);
        if (r) return r;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Aggregator helpers
// ---------------------------------------------------------------------------

std::string normalize_model_for_provider(const std::string& model,
                                         const std::string& provider) {
    std::string m = trim(model);
    if (m.empty()) return m;
    bool aggregator = providers_cmd::is_aggregator(provider);
    auto slash = m.find('/');
    if (aggregator) {
        // Aggregators use "vendor/model" slugs — keep as-is.
        return m;
    }
    if (slash != std::string::npos) {
        // Non-aggregator: strip vendor prefix (openrouter/foo -> foo).
        return m.substr(slash + 1);
    }
    return m;
}

std::string aggregator_catalog_lookup(const std::string& model,
                                      const std::vector<std::string>& catalog) {
    std::string target = to_lower(model);
    // Exact match first.
    for (const auto& mid : catalog) {
        if (to_lower(mid) == target) return mid;
    }
    // Bare name match ("gpt-5" against "openai/gpt-5").
    for (const auto& mid : catalog) {
        auto slash = mid.find('/');
        if (slash == std::string::npos) continue;
        std::string bare = mid.substr(slash + 1);
        if (to_lower(bare) == target) return mid;
    }
    return model;
}

std::string convert_vendor_colon_to_slash(const std::string& raw_input,
                                          bool is_aggregator) {
    if (!is_aggregator) return raw_input;
    auto colon = raw_input.find(':');
    if (colon == 0 || colon == std::string::npos) return raw_input;
    if (raw_input.find('/') != std::string::npos) return raw_input;
    std::string left = to_lower(trim(raw_input.substr(0, colon)));
    std::string right = trim(raw_input.substr(colon + 1));
    if (left.empty() || right.empty()) return raw_input;
    return left + "/" + right;
}

std::vector<std::string> fallback_chain(
    const std::string& primary,
    const std::vector<std::string>& authenticated_providers) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string& p) {
        if (p.empty()) return;
        if (seen.insert(p).second) out.push_back(p);
    };
    push(primary);
    for (const auto& p : authenticated_providers) push(p);
    return out;
}

// ---------------------------------------------------------------------------
// Core model-switching pipeline (pure)
// ---------------------------------------------------------------------------

ModelSwitchResult switch_model(const ModelSwitchInputs& in) {
    ModelSwitchResult r;
    r.is_global = in.is_global;

    std::string new_model = trim(in.raw_input);
    std::string target_provider = in.current_provider;
    std::string resolved_alias;

    auto find_catalog = [&](const std::string& prov) -> const std::vector<std::string>& {
        auto it = in.catalogs.find(prov);
        static const std::vector<std::string> empty;
        return (it != in.catalogs.end()) ? it->second : empty;
    };

    // ---------------- PATH A: Explicit --provider ----------------
    if (!in.explicit_provider.empty()) {
        auto pdef = providers_cmd::resolve_provider_full(
            in.explicit_provider, in.user_providers, in.custom_providers);
        if (!pdef) {
            r.success = false;
            r.error_message = "Unknown provider '" + in.explicit_provider +
                              "'. Check 'hermes model' for available providers, "
                              "or define it in config.yaml under 'providers:'.";
            return r;
        }
        target_provider = pdef->id;
        r.provider_label = pdef->name;

        if (new_model.empty()) {
            // No model given — we can't auto-detect without a running endpoint;
            // surface a helpful error that callers can short-circuit on.
            if (pdef->base_url.empty()) {
                r.success = false;
                r.target_provider = target_provider;
                r.error_message =
                    "Provider '" + pdef->name + "' has no base URL configured. "
                    "Specify a model: /model <model-name> --provider " +
                    in.explicit_provider;
                return r;
            }
            r.success = false;
            r.target_provider = target_provider;
            r.error_message =
                "No model specified for provider '" + pdef->name +
                "'. Specify the model explicitly: /model <model-name> --provider " +
                in.explicit_provider;
            return r;
        }

        // Resolve alias on the TARGET provider.
        auto alias_result = resolve_alias(new_model, target_provider,
                                          find_catalog(target_provider),
                                          in.direct_aliases);
        if (alias_result) {
            new_model = alias_result->model;
            resolved_alias = alias_result->alias_name;
        }
    }
    // ---------------- PATH B: No explicit provider ----------------
    else {
        auto alias_result = resolve_alias(in.raw_input, in.current_provider,
                                          find_catalog(in.current_provider),
                                          in.direct_aliases);
        if (alias_result) {
            target_provider = alias_result->provider;
            new_model = alias_result->model;
            resolved_alias = alias_result->alias_name;
        } else {
            std::string key = to_lower(trim(in.raw_input));
            auto mit = model_aliases().find(key);
            if (mit != model_aliases().end()) {
                // Fallback to authed providers.
                std::vector<std::string> authed;  // caller can supply via catalogs
                for (const auto& kv : in.catalogs) authed.push_back(kv.first);
                auto fb = resolve_alias_fallback(in.raw_input, authed,
                                                 in.catalogs, in.direct_aliases);
                if (fb) {
                    target_provider = fb->provider;
                    new_model = fb->model;
                    resolved_alias = fb->alias_name;
                } else {
                    r.success = false;
                    r.error_message =
                        "Alias '" + key + "' maps to " + mit->second.vendor + "/" +
                        mit->second.family + " but no matching model was found "
                        "in any provider catalog. Try specifying the full model name.";
                    return r;
                }
            } else {
                // Aggregator-only vendor:model → vendor/model conversion.
                bool agg = providers_cmd::is_aggregator(in.current_provider);
                new_model = convert_vendor_colon_to_slash(in.raw_input, agg);
            }
        }

        // Aggregator catalog search (when no alias fired).
        if (providers_cmd::is_aggregator(target_provider) && resolved_alias.empty()) {
            const auto& cat = find_catalog(target_provider);
            if (!cat.empty()) new_model = aggregator_catalog_lookup(new_model, cat);
        }
    }

    // ---------------- COMMON: label + normalisation ----------------
    bool provider_changed = target_provider != in.current_provider;
    std::string label = providers_cmd::get_label(target_provider);
    if (!label.empty()) r.provider_label = label;

    // Custom provider id — resolve via resolve_provider_full for the
    // nicer display name.
    if (target_provider.rfind("custom:", 0) == 0) {
        auto cp = providers_cmd::resolve_provider_full(
            target_provider, in.user_providers, in.custom_providers);
        if (cp) r.provider_label = cp->name;
    }

    // Direct-alias base_url override: when the alias fired, honour the
    // alias's exact base_url (so e.g. ollama.com endpoints stick).
    std::string base_url = in.current_base_url;
    std::string api_key = in.current_api_key;
    if (!resolved_alias.empty()) {
        auto da_it = in.direct_aliases.find(resolved_alias);
        if (da_it != in.direct_aliases.end() && !da_it->second.base_url.empty()) {
            base_url = da_it->second.base_url;
            if (api_key.empty()) api_key = "no-key-required";
        }
    }

    // Normalise model name for target provider.
    new_model = normalize_model_for_provider(new_model, target_provider);

    // Warnings.
    std::string hermes_warn = check_hermes_model_warning(new_model);

    // Build result.
    r.success = true;
    r.new_model = new_model;
    r.target_provider = target_provider;
    r.provider_changed = provider_changed;
    r.api_key = api_key;
    r.base_url = base_url;
    r.api_mode = providers_cmd::determine_api_mode(target_provider, base_url);
    r.resolved_via_alias = resolved_alias;
    if (!hermes_warn.empty()) r.warning_message = hermes_warn;
    return r;
}

}  // namespace hermes::cli::model_switch_cmd
