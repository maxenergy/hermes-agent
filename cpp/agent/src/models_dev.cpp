// C++17 port of agent/models_dev.py.  Parses the models.dev catalog and
// exposes rich dataclass-style accessors.
#include "hermes/agent/models_dev.hpp"

#include "hermes/core/logging.hpp"
#include "hermes/core/path.hpp"
#include "hermes/llm/llm_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace hermes::agent::models_dev {

namespace {

// ---------------------------------------------------------------------------
// State — in-memory cache and transport override.
// ---------------------------------------------------------------------------

std::mutex& state_mutex() {
    static std::mutex m;
    return m;
}

nlohmann::json& cached_catalog() {
    static nlohmann::json c = nlohmann::json::object();
    return c;
}

std::int64_t& cached_time() {
    static std::int64_t t = 0;
    return t;
}

std::optional<nlohmann::json>& injected_catalog() {
    static std::optional<nlohmann::json> c;
    return c;
}

hermes::llm::HttpTransport*& transport_override() {
    static hermes::llm::HttpTransport* t = nullptr;
    return t;
}

std::int64_t now_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(
               steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// String helpers.
// ---------------------------------------------------------------------------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool write_file_atomic(const std::filesystem::path& p,
                       const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
        if (!ofs) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

// Convert a numeric JSON value to int64; non-numeric / zero / negative
// returns 0.  Accepts integer and floating-point JSON values.
std::int64_t extract_positive_int(const nlohmann::json& v) {
    if (v.is_number_integer()) {
        auto x = v.get<long long>();
        return x > 0 ? x : 0;
    }
    if (v.is_number_unsigned()) {
        auto x = v.get<unsigned long long>();
        return x > 0 ? static_cast<std::int64_t>(x) : 0;
    }
    if (v.is_number_float()) {
        auto x = v.get<double>();
        return x > 0 ? static_cast<std::int64_t>(x) : 0;
    }
    return 0;
}

double extract_double(const nlohmann::json& v, double fallback = 0.0) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

std::string get_str(const nlohmann::json& obj, const char* key,
                    const std::string& fallback = {}) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    return fallback;
}

bool get_bool(const nlohmann::json& obj, const char* key, bool fallback) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    return fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// Provider mapping — static data tables.
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, std::string>&
provider_to_models_dev() {
    static const std::unordered_map<std::string, std::string> m = {
        {"openrouter", "openrouter"},
        {"anthropic", "anthropic"},
        {"zai", "zai"},
        {"kimi-coding", "kimi-for-coding"},
        {"minimax", "minimax"},
        {"minimax-cn", "minimax-cn"},
        {"deepseek", "deepseek"},
        {"alibaba", "alibaba"},
        {"qwen-oauth", "alibaba"},
        {"copilot", "github-copilot"},
        {"ai-gateway", "vercel"},
        {"opencode-zen", "opencode"},
        {"opencode-go", "opencode-go"},
        {"kilocode", "kilo"},
        {"fireworks", "fireworks-ai"},
        {"huggingface", "huggingface"},
        {"gemini", "google"},
        {"google", "google"},
        {"xai", "xai"},
        {"nvidia", "nvidia"},
        {"groq", "groq"},
        {"mistral", "mistral"},
        {"togetherai", "togetherai"},
        {"perplexity", "perplexity"},
        {"cohere", "cohere"},
    };
    return m;
}

const std::unordered_map<std::string, std::string>&
models_dev_to_provider() {
    static const auto build = []() {
        std::unordered_map<std::string, std::string> rev;
        for (const auto& kv : provider_to_models_dev()) {
            rev.emplace(kv.second, kv.first);
        }
        return rev;
    };
    static const std::unordered_map<std::string, std::string> m = build();
    return m;
}

// ---------------------------------------------------------------------------
// ModelInfo / ProviderInfo methods.
// ---------------------------------------------------------------------------

bool ModelInfo::has_cost_data() const {
    return cost_input > 0.0 || cost_output > 0.0;
}

bool ModelInfo::supports_vision() const {
    if (attachment) return true;
    for (const auto& m : input_modalities) {
        if (m == "image") return true;
    }
    return false;
}

bool ModelInfo::supports_pdf() const {
    for (const auto& m : input_modalities) {
        if (m == "pdf") return true;
    }
    return false;
}

bool ModelInfo::supports_audio_input() const {
    for (const auto& m : input_modalities) {
        if (m == "audio") return true;
    }
    return false;
}

std::string ModelInfo::format_cost() const {
    if (!has_cost_data()) return "unknown";
    char buf[64];
    std::string out;
    std::snprintf(buf, sizeof(buf), "$%.2f/M in", cost_input);
    out = buf;
    std::snprintf(buf, sizeof(buf), ", $%.2f/M out", cost_output);
    out += buf;
    if (cost_cache_read) {
        std::snprintf(buf, sizeof(buf), ", cache read $%.2f/M",
                      *cost_cache_read);
        out += buf;
    }
    return out;
}

std::string ModelInfo::format_capabilities() const {
    std::vector<std::string> caps;
    if (reasoning) caps.push_back("reasoning");
    if (tool_call) caps.push_back("tools");
    if (supports_vision()) caps.push_back("vision");
    if (supports_pdf()) caps.push_back("PDF");
    if (supports_audio_input()) caps.push_back("audio");
    if (structured_output) caps.push_back("structured output");
    if (open_weights) caps.push_back("open weights");
    if (caps.empty()) return "basic";
    std::string out = caps.front();
    for (std::size_t i = 1; i < caps.size(); ++i) {
        out += ", ";
        out += caps[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Cache control API.
// ---------------------------------------------------------------------------

void set_transport(hermes::llm::HttpTransport* transport) {
    std::lock_guard<std::mutex> g(state_mutex());
    transport_override() = transport;
}

void set_injected_catalog(std::optional<nlohmann::json> catalog) {
    std::lock_guard<std::mutex> g(state_mutex());
    injected_catalog() = std::move(catalog);
    // Clearing the in-memory cache forces the next fetch to reach the
    // injected value (or fall back through disk/network as configured).
    cached_catalog() = nlohmann::json::object();
    cached_time() = 0;
}

void reset_memory_cache() {
    std::lock_guard<std::mutex> g(state_mutex());
    cached_catalog() = nlohmann::json::object();
    cached_time() = 0;
}

// ---------------------------------------------------------------------------
// fetch_models_dev — memory → injected → disk → network.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path cache_path() {
    return hermes::core::path::get_hermes_home() / "models_dev_cache.json";
}

nlohmann::json load_disk_cache() {
    auto p = cache_path();
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return nlohmann::json::object();
    auto text = read_file(p);
    if (text.empty()) return nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(text);
        if (parsed.is_object()) return parsed;
    } catch (...) {}
    return nlohmann::json::object();
}

void save_disk_cache(const nlohmann::json& data) {
    try {
        write_file_atomic(cache_path(), data.dump());
    } catch (...) {}
}

hermes::llm::HttpTransport* effective_transport() {
    if (auto* t = transport_override()) return t;
    return hermes::llm::get_default_transport();
}

nlohmann::json network_fetch() {
    auto* t = effective_transport();
    if (!t) return {};
    std::unordered_map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    try {
        auto resp = t->get(kModelsDevUrl, headers);
        if (resp.status_code < 200 || resp.status_code >= 300) {
            return {};
        }
        auto parsed = nlohmann::json::parse(resp.body);
        if (parsed.is_object()) return parsed;
    } catch (const std::exception& e) {
        hermes::core::logging::log_debug(
            std::string("models_dev.network_fetch: ") + e.what());
    } catch (...) {
    }
    return {};
}

}  // namespace

nlohmann::json fetch_models_dev(bool force_refresh) {
    {
        std::lock_guard<std::mutex> g(state_mutex());
        if (injected_catalog()) return *injected_catalog();
        if (!force_refresh && !cached_catalog().empty() &&
            (now_seconds() - cached_time()) < kModelsDevCacheTtlSeconds) {
            return cached_catalog();
        }
    }

    auto fetched = network_fetch();
    if (fetched.is_object() && !fetched.empty()) {
        std::lock_guard<std::mutex> g(state_mutex());
        cached_catalog() = fetched;
        cached_time() = now_seconds();
        save_disk_cache(fetched);
        return fetched;
    }

    // Fall back to disk cache.
    std::lock_guard<std::mutex> g(state_mutex());
    if (cached_catalog().empty()) {
        auto disk = load_disk_cache();
        if (!disk.empty()) {
            cached_catalog() = disk;
            // Short TTL (5 min) so we retry the network fetch soon.
            cached_time() = now_seconds() - kModelsDevCacheTtlSeconds + 300;
        }
    }
    return cached_catalog();
}

// ---------------------------------------------------------------------------
// Extraction helpers.
// ---------------------------------------------------------------------------

namespace {

std::int64_t extract_context(const nlohmann::json& entry) {
    if (!entry.is_object()) return 0;
    auto limit_it = entry.find("limit");
    if (limit_it == entry.end() || !limit_it->is_object()) return 0;
    auto ctx_it = limit_it->find("context");
    if (ctx_it == limit_it->end()) return 0;
    return extract_positive_int(*ctx_it);
}

const nlohmann::json* provider_models_dict(const nlohmann::json& catalog,
                                           const std::string& hermes_prov) {
    const auto& map = provider_to_models_dev();
    auto it = map.find(hermes_prov);
    if (it == map.end()) return nullptr;
    auto p_it = catalog.find(it->second);
    if (p_it == catalog.end() || !p_it->is_object()) return nullptr;
    auto m_it = p_it->find("models");
    if (m_it == p_it->end() || !m_it->is_object()) return nullptr;
    return &*m_it;
}

const nlohmann::json* find_model_entry(const nlohmann::json& models,
                                       const std::string& model) {
    auto exact = models.find(model);
    if (exact != models.end() && exact->is_object()) return &*exact;
    auto lower = to_lower(model);
    for (auto it = models.begin(); it != models.end(); ++it) {
        if (!it->is_object()) continue;
        if (to_lower(it.key()) == lower) return &*it;
    }
    return nullptr;
}

}  // namespace

std::int64_t lookup_models_dev_context(const std::string& provider,
                                       const std::string& model) {
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return 0;
    const nlohmann::json* models = provider_models_dict(catalog, provider);
    if (!models) return 0;
    const nlohmann::json* entry = find_model_entry(*models, model);
    if (!entry) return 0;
    return extract_context(*entry);
}

std::optional<ModelCapabilities> get_model_capabilities(
    const std::string& provider, const std::string& model) {
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return std::nullopt;
    const nlohmann::json* models = provider_models_dict(catalog, provider);
    if (!models) return std::nullopt;
    const nlohmann::json* entry = find_model_entry(*models, model);
    if (!entry) return std::nullopt;

    ModelCapabilities c;
    c.supports_tools = get_bool(*entry, "tool_call", false);
    c.supports_vision = get_bool(*entry, "attachment", false);
    c.supports_reasoning = get_bool(*entry, "reasoning", false);
    auto limit_it = entry->find("limit");
    if (limit_it != entry->end() && limit_it->is_object()) {
        if (auto ctx = limit_it->find("context"); ctx != limit_it->end()) {
            auto v = extract_positive_int(*ctx);
            if (v > 0) c.context_window = v;
        }
        if (auto out = limit_it->find("output"); out != limit_it->end()) {
            auto v = extract_positive_int(*out);
            if (v > 0) c.max_output_tokens = v;
        }
    }
    c.model_family = get_str(*entry, "family");
    return c;
}

std::vector<std::string> list_provider_models(const std::string& provider) {
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return {};
    const nlohmann::json* models = provider_models_dict(catalog, provider);
    if (!models) return {};
    std::vector<std::string> out;
    out.reserve(models->size());
    for (auto it = models->begin(); it != models->end(); ++it) {
        out.push_back(it.key());
    }
    return out;
}

bool is_noise_model_id(const std::string& model_id) {
    static const std::regex kPat(
        R"(-tts\b|embedding|live-|-(preview|exp)-\d{2,4}[-_]|)"
        R"(-image\b|-image-preview\b|-customtools\b)",
        std::regex::icase);
    return std::regex_search(model_id, kPat);
}

std::vector<std::string> list_agentic_models(const std::string& provider) {
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return {};
    const nlohmann::json* models = provider_models_dict(catalog, provider);
    if (!models) return {};
    std::vector<std::string> out;
    for (auto it = models->begin(); it != models->end(); ++it) {
        if (!it->is_object()) continue;
        if (!get_bool(*it, "tool_call", false)) continue;
        if (is_noise_model_id(it.key())) continue;
        out.push_back(it.key());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Fuzzy search — substring first, then edit-distance ranking.
// ---------------------------------------------------------------------------

namespace {

// Compute an edit-distance-based similarity ratio in [0, 1].  Mirrors
// difflib.SequenceMatcher.ratio() closely enough for our ranking needs
// (we only care about ordering and the cutoff).
double similarity_ratio(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 1.0;
    // Classic DP Levenshtein distance.
    std::size_t n = a.size();
    std::size_t m = b.size();
    if (n == 0) return 0.0;
    if (m == 0) return 0.0;
    std::vector<std::vector<std::size_t>> dp(
        n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = 0; i <= n; ++i) dp[i][0] = i;
    for (std::size_t j = 0; j <= m; ++j) dp[0][j] = j;
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            auto cost = (a[i - 1] == b[j - 1]) ? std::size_t{0} : std::size_t{1};
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + cost});
        }
    }
    double dist = static_cast<double>(dp[n][m]);
    double maxlen = static_cast<double>(std::max(n, m));
    return 1.0 - dist / maxlen;
}

}  // namespace

std::vector<SearchResult> search_models_dev(const std::string& query,
                                            const std::string& provider,
                                            std::size_t limit) {
    auto catalog = fetch_models_dev();
    if (!catalog.is_object() || catalog.empty()) return {};

    struct Candidate {
        std::string provider;
        std::string model_id;
        nlohmann::json entry;
    };
    std::vector<Candidate> candidates;

    auto collect_one = [&](const std::string& hermes_prov,
                           const std::string& mdev_prov) {
        auto p_it = catalog.find(mdev_prov);
        if (p_it == catalog.end() || !p_it->is_object()) return;
        auto m_it = p_it->find("models");
        if (m_it == p_it->end() || !m_it->is_object()) return;
        for (auto it = m_it->begin(); it != m_it->end(); ++it) {
            candidates.push_back({hermes_prov, it.key(), *it});
        }
    };

    if (!provider.empty()) {
        const auto& map = provider_to_models_dev();
        auto it = map.find(provider);
        if (it == map.end()) return {};
        collect_one(provider, it->second);
    } else {
        for (const auto& kv : provider_to_models_dev()) {
            collect_one(kv.first, kv.second);
        }
    }

    if (candidates.empty()) return {};

    auto query_lower = to_lower(query);

    std::set<std::pair<std::string, std::string>> seen;
    std::vector<SearchResult> results;

    // Substring matches first.
    for (const auto& c : candidates) {
        if (to_lower(c.model_id).find(query_lower) != std::string::npos) {
            auto key = std::make_pair(c.provider, c.model_id);
            if (seen.insert(key).second) {
                results.push_back({c.provider, c.model_id, c.entry});
                if (results.size() >= limit) return results;
            }
        }
    }

    // Edit-distance-ranked fallback (cutoff 0.4).
    struct Ranked {
        const Candidate* cand;
        double score;
    };
    std::vector<Ranked> ranked;
    for (const auto& c : candidates) {
        double s = similarity_ratio(query_lower, to_lower(c.model_id));
        if (s >= 0.4) ranked.push_back({&c, s});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) {
                  return a.score > b.score;
              });
    for (const auto& r : ranked) {
        auto key = std::make_pair(r.cand->provider, r.cand->model_id);
        if (seen.insert(key).second) {
            results.push_back(
                {r.cand->provider, r.cand->model_id, r.cand->entry});
            if (results.size() >= limit) return results;
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// Rich dataclass constructors.
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> to_string_vec(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& v : j) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

}  // namespace

ModelInfo parse_model_info(const std::string& model_id,
                           const nlohmann::json& raw,
                           const std::string& provider_id) {
    ModelInfo mi;
    mi.id = model_id;
    mi.provider_id = provider_id;
    mi.name = get_str(raw, "name", model_id);
    if (mi.name.empty()) mi.name = model_id;
    mi.family = get_str(raw, "family");
    mi.reasoning = get_bool(raw, "reasoning", false);
    mi.tool_call = get_bool(raw, "tool_call", false);
    mi.attachment = get_bool(raw, "attachment", false);
    mi.temperature = get_bool(raw, "temperature", false);
    mi.structured_output = get_bool(raw, "structured_output", false);
    mi.open_weights = get_bool(raw, "open_weights", false);

    if (auto mod_it = raw.find("modalities");
        mod_it != raw.end() && mod_it->is_object()) {
        if (auto inp = mod_it->find("input"); inp != mod_it->end()) {
            mi.input_modalities = to_string_vec(*inp);
        }
        if (auto out = mod_it->find("output"); out != mod_it->end()) {
            mi.output_modalities = to_string_vec(*out);
        }
    }

    if (auto lim_it = raw.find("limit");
        lim_it != raw.end() && lim_it->is_object()) {
        if (auto ctx = lim_it->find("context"); ctx != lim_it->end()) {
            mi.context_window = extract_positive_int(*ctx);
        }
        if (auto out = lim_it->find("output"); out != lim_it->end()) {
            mi.max_output = extract_positive_int(*out);
        }
        if (auto inp = lim_it->find("input"); inp != lim_it->end()) {
            auto v = extract_positive_int(*inp);
            if (v > 0) mi.max_input = v;
        }
    }

    if (auto cost_it = raw.find("cost");
        cost_it != raw.end() && cost_it->is_object()) {
        if (auto v = cost_it->find("input"); v != cost_it->end()) {
            mi.cost_input = extract_double(*v, 0.0);
        }
        if (auto v = cost_it->find("output"); v != cost_it->end()) {
            mi.cost_output = extract_double(*v, 0.0);
        }
        if (auto v = cost_it->find("cache_read");
            v != cost_it->end() && !v->is_null()) {
            mi.cost_cache_read = extract_double(*v, 0.0);
        }
        if (auto v = cost_it->find("cache_write");
            v != cost_it->end() && !v->is_null()) {
            mi.cost_cache_write = extract_double(*v, 0.0);
        }
    }

    mi.knowledge_cutoff = get_str(raw, "knowledge");
    mi.release_date = get_str(raw, "release_date");
    mi.status = get_str(raw, "status");
    if (auto il = raw.find("interleaved"); il != raw.end()) {
        mi.interleaved = *il;
    }
    return mi;
}

ProviderInfo parse_provider_info(const std::string& provider_id,
                                 const nlohmann::json& raw) {
    ProviderInfo pi;
    pi.id = provider_id;
    pi.name = get_str(raw, "name", provider_id);
    if (pi.name.empty()) pi.name = provider_id;
    if (auto env_it = raw.find("env"); env_it != raw.end()) {
        pi.env = to_string_vec(*env_it);
    }
    pi.api = get_str(raw, "api");
    pi.doc = get_str(raw, "doc");
    if (auto m_it = raw.find("models");
        m_it != raw.end() && m_it->is_object()) {
        pi.model_count = m_it->size();
    }
    return pi;
}

std::optional<ProviderInfo> get_provider_info(
    const std::string& provider_id) {
    const auto& map = provider_to_models_dev();
    std::string mdev_id;
    if (auto it = map.find(provider_id); it != map.end()) {
        mdev_id = it->second;
    } else {
        mdev_id = provider_id;
    }
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return std::nullopt;
    auto p_it = catalog.find(mdev_id);
    if (p_it == catalog.end() || !p_it->is_object()) return std::nullopt;
    return parse_provider_info(mdev_id, *p_it);
}

std::optional<ModelInfo> get_model_info(const std::string& provider_id,
                                        const std::string& model_id) {
    const auto& map = provider_to_models_dev();
    std::string mdev_id;
    if (auto it = map.find(provider_id); it != map.end()) {
        mdev_id = it->second;
    } else {
        mdev_id = provider_id;
    }
    auto catalog = fetch_models_dev();
    if (!catalog.is_object()) return std::nullopt;
    auto p_it = catalog.find(mdev_id);
    if (p_it == catalog.end() || !p_it->is_object()) return std::nullopt;
    auto m_it = p_it->find("models");
    if (m_it == p_it->end() || !m_it->is_object()) return std::nullopt;

    const nlohmann::json* entry = find_model_entry(*m_it, model_id);
    if (!entry) return std::nullopt;
    // Preserve the original case key when we matched case-insensitively.
    std::string actual_id = model_id;
    if (m_it->find(model_id) == m_it->end()) {
        auto lower = to_lower(model_id);
        for (auto it = m_it->begin(); it != m_it->end(); ++it) {
            if (to_lower(it.key()) == lower) {
                actual_id = it.key();
                break;
            }
        }
    }
    return parse_model_info(actual_id, *entry, mdev_id);
}

}  // namespace hermes::agent::models_dev
