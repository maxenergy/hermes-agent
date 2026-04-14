// agent/models_dev — C++17 port of agent/models_dev.py.
//
// Parses the models.dev catalog (a community-maintained JSON database of
// providers and models) and exposes rich dataclass-style accessors.  Data
// resolution mirrors the Python module: in-memory cache (1h TTL) → disk
// cache (~/.hermes/models_dev_cache.json) → network fetch.
//
// The C++ version injects the HTTP transport so tests can stub the network
// fetch deterministically; the default transport uses libcurl (PRIVATE dep
// via hermes::llm).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::llm {
class HttpTransport;
}  // namespace hermes::llm

namespace hermes::agent::models_dev {

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

inline constexpr const char* kModelsDevUrl = "https://models.dev/api.json";
inline constexpr int kModelsDevCacheTtlSeconds = 3600;

// ---------------------------------------------------------------------------
// Provider ID mapping (Hermes → models.dev).
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, std::string>& provider_to_models_dev();
const std::unordered_map<std::string, std::string>& models_dev_to_provider();

// ---------------------------------------------------------------------------
// Data types.
// ---------------------------------------------------------------------------

struct ModelInfo {
    std::string id;
    std::string name;
    std::string family;
    std::string provider_id;  // models.dev provider ID

    // Capability flags.
    bool reasoning = false;
    bool tool_call = false;
    bool attachment = false;
    bool temperature = false;
    bool structured_output = false;
    bool open_weights = false;

    // Modalities.
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;

    // Limits.
    std::int64_t context_window = 0;
    std::int64_t max_output = 0;
    std::optional<std::int64_t> max_input;

    // Cost (per million tokens, USD).
    double cost_input = 0.0;
    double cost_output = 0.0;
    std::optional<double> cost_cache_read;
    std::optional<double> cost_cache_write;

    // Metadata.
    std::string knowledge_cutoff;
    std::string release_date;
    std::string status;
    nlohmann::json interleaved = false;

    bool has_cost_data() const;
    bool supports_vision() const;
    bool supports_pdf() const;
    bool supports_audio_input() const;
    std::string format_cost() const;
    std::string format_capabilities() const;
};

struct ProviderInfo {
    std::string id;
    std::string name;
    std::vector<std::string> env;
    std::string api;
    std::string doc;
    std::size_t model_count = 0;
};

struct ModelCapabilities {
    bool supports_tools = true;
    bool supports_vision = false;
    bool supports_reasoning = false;
    std::int64_t context_window = 200000;
    std::int64_t max_output_tokens = 8192;
    std::string model_family;
};

struct SearchResult {
    std::string provider;
    std::string model_id;
    nlohmann::json entry;
};

// ---------------------------------------------------------------------------
// Cache control.
// ---------------------------------------------------------------------------

// Override the HTTP transport.  Pass nullptr to restore the default.
void set_transport(hermes::llm::HttpTransport* transport);

// Inject a pre-fetched catalog (skips disk + network entirely).
// Clears with ``nullopt``.
void set_injected_catalog(std::optional<nlohmann::json> catalog);

// Reset in-memory cache (does not touch the disk cache).
void reset_memory_cache();

// ---------------------------------------------------------------------------
// Fetch / query API.
// ---------------------------------------------------------------------------

// Return the full registry dict.  Respects in-memory TTL + disk fallback.
nlohmann::json fetch_models_dev(bool force_refresh = false);

// Look up context window (tokens) for a provider+model combo.  Returns 0
// when the entry is missing, invalid, or reports context=0.
std::int64_t lookup_models_dev_context(const std::string& provider,
                                       const std::string& model);

// Resolve capability metadata.  Returns nullopt when model not found.
std::optional<ModelCapabilities> get_model_capabilities(
    const std::string& provider, const std::string& model);

// All model IDs for a provider.
std::vector<std::string> list_provider_models(const std::string& provider);

// Model IDs suitable for agentic use (filters tool_call=true + noise).
std::vector<std::string> list_agentic_models(const std::string& provider);

// Fuzzy search across the catalog (substring match → difflib-style
// close-match fallback).  ``provider`` empty ⇒ search all providers.
std::vector<SearchResult> search_models_dev(const std::string& query,
                                            const std::string& provider = {},
                                            std::size_t limit = 5);

// Rich dataclass constructors.
ModelInfo parse_model_info(const std::string& model_id,
                           const nlohmann::json& raw,
                           const std::string& provider_id);
ProviderInfo parse_provider_info(const std::string& provider_id,
                                 const nlohmann::json& raw);

std::optional<ProviderInfo> get_provider_info(const std::string& provider_id);
std::optional<ModelInfo> get_model_info(const std::string& provider_id,
                                        const std::string& model_id);

// ---------------------------------------------------------------------------
// Noise filter (exposed for testing).
// ---------------------------------------------------------------------------

// Returns true when the model ID matches one of the noise patterns
// (TTS / embedding / dated preview snapshot / live / image-only).
bool is_noise_model_id(const std::string& model_id);

}  // namespace hermes::agent::models_dev
