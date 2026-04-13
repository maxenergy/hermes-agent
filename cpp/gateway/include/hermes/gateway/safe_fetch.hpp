// Phase 12 — Gateway-side wrapper around HttpTransport that enforces
// SSRF protection on all outbound URLs.  Adapters that must fetch
// arbitrary user-provided URLs (file attachment downloads, link
// previews, OAuth authorize redirects) should route through this
// helper so a single guard rejects RFC1918 / loopback / cloud
// metadata targets.
#pragma once

#include <string>
#include <unordered_map>

#include <hermes/llm/llm_client.hpp>

namespace hermes::gateway {

struct SafeFetchResult {
    bool ok = false;
    int status_code = 0;
    std::string body;
    std::string error;  // populated on ssrf reject / transport throw
};

// GET a URL through the supplied transport (or the default transport
// when nullptr).  Returns ok=false with error="ssrf_blocked" when the
// host resolves to a private or metadata address.
SafeFetchResult safe_get(const std::string& url,
                         const std::unordered_map<std::string, std::string>&
                             headers = {},
                         hermes::llm::HttpTransport* transport = nullptr);

// POST JSON to a URL through the same SSRF guard.
SafeFetchResult safe_post_json(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    hermes::llm::HttpTransport* transport = nullptr);

}  // namespace hermes::gateway
