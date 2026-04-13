// Phase 8: HTTP-backed web tools — web_search and web_extract.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <cstddef>

namespace hermes::tools {

// Register web_search and web_extract into the global ToolRegistry.
// |transport| is used for HTTP calls; may be nullptr (tools will return
// an error at dispatch time).
void register_web_tools(hermes::llm::HttpTransport* transport);

// Test hooks for the in-process search TTL cache.
void clear_web_search_cache();
void set_web_search_cache_ttl_seconds(int ttl);
std::size_t web_search_cache_size();

}  // namespace hermes::tools
