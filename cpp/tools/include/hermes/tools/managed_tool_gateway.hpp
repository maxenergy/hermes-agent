// Nous-hosted vendor gateway resolver.
//
// Each vendor tool (firecrawl, modal, exa, …) calls
// resolve_vendor_gateway() at request time.  When the managed gateway is
// enabled in config, the tool's base URL is rewritten to the returned
// proxy URL (derived from `<scheme>://<vendor>-gateway.<domain>` unless
// explicitly overridden with `managed_tool_gateway.<vendor>.url`).
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hermes::tools {

struct VendorGatewayUrl {
    std::string vendor;     // "firecrawl", "modal", "exa", ...
    std::string proxy_url;  // "https://firecrawl-gateway.nousresearch.com"
    bool enabled = false;
};

// Walk the supplied config tree and return a VendorGatewayUrl when the
// managed gateway is enabled AND the vendor has either an explicit
// override URL OR a default origin can be derived.
//
// Recognized config keys:
//   managed_tool_gateway.enabled            (bool)
//   managed_tool_gateway.scheme             ("http" | "https")
//   managed_tool_gateway.domain             ("nousresearch.com")
//   managed_tool_gateway.<vendor>.url       (string)
std::optional<VendorGatewayUrl> resolve_vendor_gateway(
    const std::string& vendor, const nlohmann::json& config);

// Cheap top-level check — true if managed_tool_gateway.enabled is set.
bool is_managed_nous_tools_enabled(const nlohmann::json& config);

}  // namespace hermes::tools
