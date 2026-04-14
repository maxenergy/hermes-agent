// Phase 8+: HTTP-backed web tools — web_search and web_extract.
//
// web_search supports multiple provider backends, selectable via the
// HERMES_WEB_PROVIDER env var (or the `provider` argument passed to the
// tool).  Supported providers:
//
//   - "exa"       → https://api.exa.ai/search           (default, EXA_API_KEY)
//   - "tavily"    → https://api.tavily.com/search       (TAVILY_API_KEY)
//   - "parallel"  → https://api.parallel.ai/v1/search   (PARALLEL_API_KEY)
//   - "brave"     → https://api.search.brave.com/...    (BRAVE_API_KEY)
//   - "google"    → https://www.googleapis.com/...      (GOOGLE_API_KEY+CX)
//
// A small in-memory LRU/TTL cache (60 s) is kept keyed by
// (provider, query, options_hash) to avoid hitting the network for the
// same lookup within a session.
#include "hermes/tools/web_tools.hpp"
#include "hermes/tools/registry.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::tools {

namespace {

hermes::llm::HttpTransport* g_web_transport = nullptr;

// ── In-memory TTL cache ───────────────────────────────────────────────
struct CacheEntry {
    std::chrono::steady_clock::time_point expires_at;
    std::string value;
};

class SearchCache {
public:
    static SearchCache& instance() {
        static SearchCache c;
        return c;
    }

    // TTL in seconds.
    void set_ttl(std::chrono::seconds ttl) {
        std::lock_guard<std::mutex> lk(mu_);
        ttl_ = ttl;
    }

    // Returns cached value for |key| if non-expired, otherwise empty.
    std::string get(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return {};
        if (std::chrono::steady_clock::now() >= it->second.expires_at) {
            map_.erase(it);
            return {};
        }
        return it->second.value;
    }

    void put(const std::string& key, std::string value) {
        std::lock_guard<std::mutex> lk(mu_);
        map_[key] = {std::chrono::steady_clock::now() + ttl_, std::move(value)};
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        map_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, CacheEntry> map_;
    std::chrono::seconds ttl_{60};
};

std::string cache_key(const std::string& provider,
                      const std::string& query,
                      const nlohmann::json& opts) {
    // Deterministic: hash (provider|query|canonical-opts-dump).
    std::string material = provider;
    material += '\x1f';
    material += query;
    material += '\x1f';
    material += opts.dump();
    // Use std::hash<std::string> for compactness — good enough for in-proc.
    auto h = std::hash<std::string>{}(material);
    return provider + ":" + std::to_string(h);
}

std::string env_or(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return fallback;
    return v;
}

// ── Provider-specific builders / parsers ──────────────────────────────

struct Request {
    std::string method = "POST";  // "GET" or "POST"
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct BuiltQuery {
    Request req;
    std::string error;  // non-empty means an error envelope should be returned
};

BuiltQuery build_exa(const std::string& query, int num_results,
                     const nlohmann::json& /*extra*/) {
    BuiltQuery q;
    auto key = env_or("EXA_API_KEY");
    if (key.empty()) { q.error = "EXA_API_KEY not set"; return q; }
    q.req.url = "https://api.exa.ai/search";
    q.req.headers["Content-Type"] = "application/json";
    q.req.headers["x-api-key"] = key;
    nlohmann::json body;
    body["query"] = query;
    body["numResults"] = num_results;
    q.req.body = body.dump();
    return q;
}

BuiltQuery build_tavily(const std::string& query, int num_results,
                        const nlohmann::json& extra) {
    BuiltQuery q;
    auto key = env_or("TAVILY_API_KEY");
    if (key.empty()) { q.error = "TAVILY_API_KEY not set"; return q; }
    q.req.url = "https://api.tavily.com/search";
    q.req.headers["Content-Type"] = "application/json";
    nlohmann::json body;
    body["api_key"] = key;
    body["query"] = query;
    body["max_results"] = num_results;
    body["search_depth"] =
        extra.value("search_depth", std::string("basic"));
    if (extra.contains("include_answer")) {
        body["include_answer"] = extra["include_answer"];
    }
    q.req.body = body.dump();
    return q;
}

BuiltQuery build_parallel(const std::string& query, int num_results,
                          const nlohmann::json& /*extra*/) {
    BuiltQuery q;
    auto key = env_or("PARALLEL_API_KEY");
    if (key.empty()) { q.error = "PARALLEL_API_KEY not set"; return q; }
    q.req.url = "https://api.parallel.ai/v1/search";
    q.req.headers["Content-Type"] = "application/json";
    q.req.headers["x-api-key"] = key;
    nlohmann::json body;
    body["objective"] = query;
    body["max_results"] = num_results;
    q.req.body = body.dump();
    return q;
}

BuiltQuery build_brave(const std::string& query, int num_results,
                       const nlohmann::json& /*extra*/) {
    BuiltQuery q;
    auto key = env_or("BRAVE_API_KEY");
    if (key.empty()) { q.error = "BRAVE_API_KEY not set"; return q; }
    q.req.method = "GET";
    // URL-encode query minimally (spaces → +).  Heavy users supply their own.
    std::string encoded;
    encoded.reserve(query.size());
    for (char c : query) {
        if (c == ' ') encoded += '+';
        else encoded += c;
    }
    q.req.url = "https://api.search.brave.com/res/v1/web/search?q=" +
                encoded + "&count=" + std::to_string(num_results);
    q.req.headers["Accept"] = "application/json";
    q.req.headers["X-Subscription-Token"] = key;
    return q;
}

BuiltQuery build_google(const std::string& query, int num_results,
                        const nlohmann::json& /*extra*/) {
    BuiltQuery q;
    auto key = env_or("GOOGLE_API_KEY");
    auto cx  = env_or("GOOGLE_CSE_ID");
    if (key.empty() || cx.empty()) {
        q.error = "GOOGLE_API_KEY and GOOGLE_CSE_ID must both be set";
        return q;
    }
    q.req.method = "GET";
    std::string encoded;
    for (char c : query) { encoded += (c == ' ') ? '+' : c; }
    q.req.url = "https://www.googleapis.com/customsearch/v1?key=" + key +
                "&cx=" + cx + "&q=" + encoded +
                "&num=" + std::to_string(num_results);
    return q;
}

// Provider-specific body parsing — always returns an object with
// `results` = array of {title, url, snippet}.
nlohmann::json parse_exa(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    if (!body.contains("results")) return out;
    for (const auto& r : body["results"]) {
        out["results"].push_back({
            {"title", r.value("title", "")},
            {"url", r.value("url", "")},
            {"snippet", r.value("text", "")},
        });
    }
    return out;
}

nlohmann::json parse_tavily(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    if (body.contains("answer")) out["answer"] = body["answer"];
    if (!body.contains("results")) return out;
    for (const auto& r : body["results"]) {
        out["results"].push_back({
            {"title", r.value("title", "")},
            {"url", r.value("url", "")},
            {"snippet", r.value("content", "")},
        });
    }
    return out;
}

nlohmann::json parse_parallel(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    // Parallel returns { "results": [ { "title", "url", "excerpts": [...] } ] }
    if (!body.contains("results")) return out;
    for (const auto& r : body["results"]) {
        std::string snippet;
        if (r.contains("excerpts") && r["excerpts"].is_array() &&
            !r["excerpts"].empty()) {
            // Join first few excerpts.
            for (std::size_t i = 0; i < r["excerpts"].size() && i < 3; ++i) {
                if (i) snippet += " … ";
                snippet += r["excerpts"][i].get<std::string>();
            }
        } else {
            snippet = r.value("snippet", r.value("content", ""));
        }
        out["results"].push_back({
            {"title", r.value("title", "")},
            {"url", r.value("url", "")},
            {"snippet", snippet},
        });
    }
    return out;
}

nlohmann::json parse_brave(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    if (!body.contains("web") || !body["web"].contains("results")) return out;
    for (const auto& r : body["web"]["results"]) {
        out["results"].push_back({
            {"title", r.value("title", "")},
            {"url", r.value("url", "")},
            {"snippet", r.value("description", "")},
        });
    }
    return out;
}

nlohmann::json parse_google(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    if (!body.contains("items")) return out;
    for (const auto& r : body["items"]) {
        out["results"].push_back({
            {"title", r.value("title", "")},
            {"url", r.value("link", "")},
            {"snippet", r.value("snippet", "")},
        });
    }
    return out;
}

std::string resolve_provider(const nlohmann::json& args) {
    if (args.contains("provider") && args["provider"].is_string()) {
        return args["provider"].get<std::string>();
    }
    auto env = env_or("HERMES_WEB_PROVIDER");
    if (!env.empty()) return env;
    return "exa";
}

// ── Main handler ──────────────────────────────────────────────────────

std::string handle_web_search(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    auto* transport = g_web_transport ? g_web_transport
                                     : hermes::llm::get_default_transport();
    assert(transport && "HTTP transport should always be available");

    const auto query = args.at("query").get<std::string>();
    const int num_results =
        args.contains("num_results") ? args["num_results"].get<int>() : 5;
    const std::string provider = resolve_provider(args);

    // Everything except query and provider contributes to the cache key.
    nlohmann::json opts = args;
    opts.erase("query");
    opts.erase("provider");
    const std::string key = cache_key(provider, query, opts);

    if (auto hit = SearchCache::instance().get(key); !hit.empty()) {
        // Parse back and mark as cached so callers can tell.
        auto parsed = nlohmann::json::parse(hit, nullptr, false);
        if (!parsed.is_discarded()) {
            parsed["cached"] = true;
            parsed["provider"] = provider;
            return tool_result(parsed);
        }
    }

    BuiltQuery built;
    if (provider == "exa")            built = build_exa(query, num_results, opts);
    else if (provider == "tavily")    built = build_tavily(query, num_results, opts);
    else if (provider == "parallel")  built = build_parallel(query, num_results, opts);
    else if (provider == "brave")     built = build_brave(query, num_results, opts);
    else if (provider == "google")    built = build_google(query, num_results, opts);
    else {
        return tool_error("unknown web_search provider",
                          {{"provider", provider}});
    }
    if (!built.error.empty()) return tool_error(built.error);

    hermes::llm::HttpTransport::Response resp;
    if (built.req.method == "GET") {
        resp = transport->get(built.req.url, built.req.headers);
    } else {
        resp = transport->post_json(built.req.url, built.req.headers,
                                    built.req.body);
    }
    if (resp.status_code != 200) {
        return tool_error(provider + " API error",
                          {{"status", resp.status_code},
                           {"body", resp.body},
                           {"provider", provider}});
    }

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded()) {
        return tool_error("malformed response from " + provider);
    }

    nlohmann::json parsed;
    if (provider == "exa")            parsed = parse_exa(body);
    else if (provider == "tavily")    parsed = parse_tavily(body);
    else if (provider == "parallel")  parsed = parse_parallel(body);
    else if (provider == "brave")     parsed = parse_brave(body);
    else                              parsed = parse_google(body);

    if (!parsed.contains("results") || !parsed["results"].is_array()) {
        return tool_error("malformed response from " + provider);
    }

    parsed["provider"] = provider;
    parsed["cached"] = false;
    // Cache the pre-flagged copy so repeat hits reflect cached=true.
    nlohmann::json to_cache = parsed;
    to_cache.erase("cached");
    SearchCache::instance().put(key, to_cache.dump());
    return tool_result(parsed);
}

std::string handle_web_extract(const nlohmann::json& args,
                               const ToolContext& /*ctx*/) {
    auto* transport = g_web_transport ? g_web_transport
                                     : hermes::llm::get_default_transport();
    assert(transport && "HTTP transport should always be available");

    const auto url = args.at("url").get<std::string>();
    const int max_length =
        args.contains("max_length") ? args["max_length"].get<int>() : 10000;

    const char* api_key = std::getenv("FIRECRAWL_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return tool_error("FIRECRAWL_API_KEY not set");
    }

    nlohmann::json req_body;
    req_body["url"] = url;
    req_body["maxLength"] = max_length;

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = std::string("Bearer ") + api_key;

    auto resp = transport->post_json(
        "https://api.firecrawl.dev/v1/scrape", headers, req_body.dump());

    if (resp.status_code != 200) {
        return tool_error("Firecrawl API error",
                          {{"status", resp.status_code},
                           {"body", resp.body}});
    }

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("data")) {
        return tool_error("malformed response from Firecrawl API");
    }

    const auto& data = body["data"];
    nlohmann::json result;
    result["content"] = data.value("markdown", data.value("content", ""));
    result["title"] = data.value("title", "");
    result["url"] = url;
    return tool_result(result);
}

// check_fn for web_search — succeed if any provider's credentials exist.
bool any_web_search_key() {
    static const char* keys[] = {"EXA_API_KEY", "TAVILY_API_KEY",
                                 "PARALLEL_API_KEY", "BRAVE_API_KEY",
                                 "GOOGLE_API_KEY"};
    for (const char* k : keys) {
        const char* v = std::getenv(k);
        if (v && v[0] != '\0') return true;
    }
    return false;
}

}  // namespace

// Exposed for tests — flushes the in-process TTL cache.
void clear_web_search_cache() { SearchCache::instance().clear(); }
void set_web_search_cache_ttl_seconds(int ttl) {
    SearchCache::instance().set_ttl(std::chrono::seconds(ttl));
}
std::size_t web_search_cache_size() { return SearchCache::instance().size(); }

void register_web_tools(hermes::llm::HttpTransport* transport) {
    g_web_transport = transport;
    auto& reg = ToolRegistry::instance();

    {
        ToolEntry e;
        e.name = "web_search";
        e.toolset = "web";
        e.description =
            "Search the web via pluggable provider "
            "(exa|tavily|parallel|brave|google).";
        e.emoji = "\xF0\x9F\x94\x8D";
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}, {"description", "Search query"}}},
              {"num_results",
               {{"type", "integer"},
                {"description", "Number of results (default 5)"}}},
              {"provider",
               {{"type", "string"},
                {"description",
                 "Provider — exa|tavily|parallel|brave|google "
                 "(default: HERMES_WEB_PROVIDER or exa)"}}}}},
            {"required", nlohmann::json::array({"query"})}};
        e.handler = handle_web_search;
        e.check_fn = [] { return any_web_search_key(); };
        reg.register_tool(std::move(e));
    }

    {
        ToolEntry e;
        e.name = "web_extract";
        e.toolset = "web";
        e.description = "Extract content from a URL via Firecrawl API";
        e.emoji = "\xF0\x9F\x93\x84";
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "URL to scrape"}}},
              {"max_length",
               {{"type", "integer"},
                {"description",
                 "Max content length in chars (default 10000)"}}}}},
            {"required", nlohmann::json::array({"url"})}};
        e.handler = handle_web_extract;
        e.check_fn = [] {
            const char* k = std::getenv("FIRECRAWL_API_KEY");
            return k && k[0] != '\0';
        };
        reg.register_tool(std::move(e));
    }
}

// ── Public helpers (hermes::tools::web) ──────────────────────────────

namespace web {

const std::vector<std::string>& supported_providers() {
    static const std::vector<std::string> s{
        "exa", "tavily", "parallel", "brave", "google",
    };
    return s;
}

bool is_supported_provider(std::string_view name) {
    const auto& p = supported_providers();
    return std::find(p.begin(), p.end(), std::string(name)) != p.end();
}

std::string url_encode(std::string_view s) {
    std::ostringstream oss;
    oss.fill('0');
    oss << std::hex;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else if (c == ' ') {
            oss << '+';
        } else {
            oss << '%' << std::setw(2)
                << static_cast<int>(uc);
        }
    }
    return oss.str();
}

nlohmann::json normalize_tavily_search(const nlohmann::json& body) {
    nlohmann::json out;
    out["results"] = nlohmann::json::array();
    if (body.contains("answer") && !body["answer"].is_null()) {
        out["answer"] = body["answer"];
    }
    if (!body.contains("results") || !body["results"].is_array()) return out;
    int i = 0;
    for (const auto& r : body["results"]) {
        nlohmann::json entry;
        entry["title"] = r.value("title", "");
        entry["url"] = r.value("url", "");
        entry["snippet"] = r.value("content", r.value("snippet", ""));
        entry["position"] = ++i;
        out["results"].push_back(std::move(entry));
    }
    return out;
}

nlohmann::json normalize_tavily_documents(const nlohmann::json& body,
                                          std::string_view fallback_url) {
    nlohmann::json docs = nlohmann::json::array();
    auto fallback = std::string(fallback_url);

    if (body.contains("results") && body["results"].is_array()) {
        for (const auto& r : body["results"]) {
            std::string url = r.value("url", fallback);
            std::string raw = r.value("raw_content", r.value("content", ""));
            nlohmann::json d;
            d["url"] = url;
            d["title"] = r.value("title", "");
            d["content"] = raw;
            d["raw_content"] = raw;
            d["metadata"] = {{"sourceURL", url},
                             {"title", r.value("title", "")}};
            docs.push_back(std::move(d));
        }
    }
    if (body.contains("failed_results") && body["failed_results"].is_array()) {
        for (const auto& f : body["failed_results"]) {
            std::string url = f.value("url", fallback);
            nlohmann::json d;
            d["url"] = url;
            d["title"] = "";
            d["content"] = "";
            d["raw_content"] = "";
            d["error"] = f.value("error", "extraction failed");
            d["metadata"] = {{"sourceURL", url}};
            docs.push_back(std::move(d));
        }
    }
    if (body.contains("failed_urls") && body["failed_urls"].is_array()) {
        for (const auto& u : body["failed_urls"]) {
            std::string url = u.is_string() ? u.get<std::string>() : u.dump();
            nlohmann::json d;
            d["url"] = url;
            d["title"] = "";
            d["content"] = "";
            d["raw_content"] = "";
            d["error"] = "extraction failed";
            d["metadata"] = {{"sourceURL", url}};
            docs.push_back(std::move(d));
        }
    }
    return docs;
}

std::string cache_key(std::string_view provider, std::string_view query,
                      const nlohmann::json& opts) {
    std::string material(provider);
    material += '\x1f';
    material.append(query);
    material += '\x1f';
    material += opts.dump();
    auto h = std::hash<std::string>{}(material);
    return std::string(provider) + ":" + std::to_string(h);
}

}  // namespace web

}  // namespace hermes::tools
