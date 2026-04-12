#include "hermes/llm/models_dev.hpp"

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hermes::llm::models_dev {

namespace {

struct CacheEntry {
    ModelMetadata metadata;
    std::chrono::steady_clock::time_point fetched_at;
};

std::mutex g_cache_mu;
std::unordered_map<std::string, CacheEntry> g_cache;

// Extract the model family from a model ID.
// "claude-3-5-sonnet-20241022" -> "claude"
// "gpt-4o" -> "gpt"
std::string extract_family(std::string_view model) {
    // Strip provider prefix like "openai/" or "anthropic/"
    auto slash = model.find('/');
    if (slash != std::string_view::npos) {
        model = model.substr(slash + 1);
    }

    // Take everything up to the first dash or digit.
    std::string family;
    for (char c : model) {
        if (c == '-' || (c >= '0' && c <= '9')) break;
        family += c;
    }
    return family.empty() ? std::string(model) : family;
}

}  // namespace

std::optional<ModelMetadata> fetch_spec(std::string_view model,
                                        std::chrono::seconds cache_ttl) {
    std::string model_key(model);

    // Check cache first.
    {
        std::lock_guard<std::mutex> lock(g_cache_mu);
        auto it = g_cache.find(model_key);
        if (it != g_cache.end()) {
            auto age = std::chrono::steady_clock::now() - it->second.fetched_at;
            if (age < cache_ttl) {
                return it->second.metadata;
            }
        }
    }

    auto* transport = get_default_transport();
    if (!transport) return std::nullopt;

    auto family = extract_family(model);
    std::string url =
        "https://raw.githubusercontent.com/anthropics/model-spec/main/models/" +
        family + ".json";

    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";

    auto resp = transport->get(url, headers);
    if (resp.status_code != 200) return std::nullopt;

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded()) return std::nullopt;

    // The response may be an object with the model directly, or an array
    // of model variants.  Try to find our exact model.
    nlohmann::json spec;
    if (body.is_array()) {
        for (const auto& item : body) {
            if (item.value("model_id", "") == std::string(model)) {
                spec = item;
                break;
            }
        }
        // If exact match not found, use first entry.
        if (spec.is_null() && !body.empty()) {
            spec = body[0];
        }
    } else if (body.is_object()) {
        spec = body;
    }

    if (spec.is_null()) return std::nullopt;

    ModelMetadata meta;
    meta.model_id = std::string(model);
    meta.family = family;
    meta.context_length = spec.value("context_length", int64_t(0));
    meta.supports_reasoning = spec.value("supports_reasoning", false);
    meta.supports_vision = spec.value("supports_vision", false);
    meta.supports_prompt_cache = spec.value("supports_prompt_cache", false);

    if (spec.contains("pricing")) {
        auto& p = spec["pricing"];
        meta.pricing.input_per_million_usd =
            p.value("input_price", p.value("input_per_million_usd", 0.0));
        meta.pricing.output_per_million_usd =
            p.value("output_price", p.value("output_per_million_usd", 0.0));
    }

    meta.source = "models_dev";

    // Cache result.
    {
        std::lock_guard<std::mutex> lock(g_cache_mu);
        g_cache[model_key] = {meta, std::chrono::steady_clock::now()};
    }

    return meta;
}

}  // namespace hermes::llm::models_dev
