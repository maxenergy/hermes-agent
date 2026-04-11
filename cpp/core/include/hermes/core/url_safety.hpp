// SSRF guard: reject URLs targeting private IPs or cloud metadata.
#pragma once

#include <string_view>

namespace hermes::core::url_safety {

// True when `host_or_ip` refers to RFC1918 / loopback / link-local /
// unique-local addresses, or known metadata hostnames.
bool is_private_address(std::string_view host_or_ip);

// Parse `url` (best-effort), extract the host, and return
// `!is_private_address(host)`. Malformed URLs yield `false`.
bool is_safe_url(std::string_view url);

}  // namespace hermes::core::url_safety
