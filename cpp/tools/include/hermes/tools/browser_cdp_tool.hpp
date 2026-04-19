// Raw Chrome DevTools Protocol (CDP) passthrough tool.
//
// Ports tools/browser_cdp_tool.py.  Exposes a single tool, ``browser_cdp``,
// that sends arbitrary CDP commands to the browser's DevTools WebSocket
// endpoint.  Gated on a reachable CDP endpoint — when the BROWSER_CDP_URL
// env var is set (via ``/browser connect``) or ``browser.cdp_url`` is
// configured in ``config.yaml``.  Browser-level methods (Target.*,
// Browser.*, Storage.*) omit ``target_id``; page-level methods attach to
// the target with ``flatten=true`` and dispatch on the returned
// ``sessionId``.
//
// Per call stateless: a fresh WebSocket connection is opened, the command
// is sent, the response is awaited, and the connection is closed.  See
// ``plans/cpp17-backend-port.md`` (Item 19) for the upstream mapping.
#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace hermes::tools {

// Canonical CDP documentation URL used in the schema description.
inline constexpr std::string_view kCdpDocsUrl =
    "https://chromedevtools.github.io/devtools-protocol/";

// Resolve the active CDP override endpoint, honouring the same precedence
// chain as the Python ``_get_cdp_override``:
//   1. ``BROWSER_CDP_URL`` environment variable
//   2. ``browser.cdp_url`` from config.yaml (loaded via hermes::config)
// Returns the trimmed endpoint string (may be empty).
std::string resolve_cdp_override();

// Classification for resolve_cdp_endpoint().
enum class CdpEndpointKind {
    Empty,
    WebSocketDirect,  // ws://host:port/devtools/... — use as-is
    NeedsDiscovery,   // ws://host:port / http(s)://host:port [/json/version]
    Unsupported,      // wss://... (TLS) — not yet supported in this port
    Invalid,
};

struct ResolvedCdpEndpoint {
    CdpEndpointKind kind{CdpEndpointKind::Empty};
    std::string ws_url;       // populated for WebSocketDirect / post-discovery
    std::string discovery_url;// http(s)://.../json/version for NeedsDiscovery
    std::string error;
};

// Classify a raw endpoint string from config/env.  Does NOT perform
// network IO; callers with an HTTP client fetch ``discovery_url`` and feed
// the response JSON back via ``extract_ws_from_version_json``.
ResolvedCdpEndpoint classify_browser_cdp_endpoint(std::string_view raw);

// Parameters accepted by the handler.  Exposed for direct testing without
// going through the registry dispatcher.
struct BrowserCdpArgs {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
    std::string target_id;  // optional — empty for browser-level methods
    std::chrono::seconds timeout = std::chrono::seconds(30);
};

// Result of a call.  ``success`` is true iff ``result`` holds the CDP
// response body; otherwise ``error`` is populated.
struct BrowserCdpResult {
    bool success{false};
    nlohmann::json result;
    std::string error;
};

// Send one CDP call over the supplied ws:// URL.  When ``args.target_id``
// is non-empty the function first calls ``Target.attachToTarget`` with
// ``flatten=true`` and dispatches ``args.method`` on the returned session.
// Pure function — safe to call from tests with a local WS server.
BrowserCdpResult cdp_call(const std::string& ws_url,
                          const BrowserCdpArgs& args);

// Registers the ``browser_cdp`` tool with the global registry.  Idempotent;
// safe to call multiple times.
void register_browser_cdp_tool();

}  // namespace hermes::tools
