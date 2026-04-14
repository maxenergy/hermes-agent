// Phase 8: HTTP-backed web tools — web_search and web_extract.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

// Register web_search and web_extract into the global ToolRegistry.
// |transport| is used for HTTP calls; may be nullptr (tools will return
// an error at dispatch time).
void register_web_tools(hermes::llm::HttpTransport* transport);

// Test hooks for the in-process search TTL cache.
void clear_web_search_cache();
void set_web_search_cache_ttl_seconds(int ttl);
std::size_t web_search_cache_size();

namespace web {

// Supported web-search provider names.
const std::vector<std::string>& supported_providers();

// Return true when |name| is a recognised provider.
bool is_supported_provider(std::string_view name);

// URL-encode a query string using RFC 3986 rules (percent-encodes
// reserved chars, spaces → "+").  The encoding is lax enough for
// search engines but strict enough not to produce invalid URLs.
std::string url_encode(std::string_view s);

// Normalize a Tavily /search response envelope into
// {results: [{title, url, snippet}]}, optionally preserving
// the Tavily 'answer' field when present.
nlohmann::json normalize_tavily_search(const nlohmann::json& body);

// Normalize a Tavily /extract response into a list of documents with
// {url, title, content, raw_content, metadata} shape.  Missing results
// are included with an 'error' field.
nlohmann::json normalize_tavily_documents(const nlohmann::json& body,
                                          std::string_view fallback_url = "");

// Hash the provider/query/options into a stable cache key.
std::string cache_key(std::string_view provider, std::string_view query,
                      const nlohmann::json& opts);

}  // namespace web

}  // namespace hermes::tools
