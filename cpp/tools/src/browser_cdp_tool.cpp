// Raw Chrome DevTools Protocol (CDP) passthrough tool.
//
// See tools/browser_cdp_tool.py for the upstream reference and
// include/hermes/tools/browser_cdp_tool.hpp for the exposed surface.

#include "hermes/tools/browser_cdp_tool.hpp"

#include "hermes/tools/browser_helpers.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/simple_ws.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace hermes::tools {

namespace {

// Monotonically-increasing CDP command id shared across calls.  Using a
// process-global counter keeps ids unique even when multiple agents share
// the same browser session.
std::atomic<int> g_cdp_call_id{1};

std::string trim_str(std::string_view s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
        char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

// Parse an HTTP URL into host / port / path.  Returns false for malformed
// input.  Defaults port to 80 / 443 based on scheme.
bool parse_http_url(std::string_view url, std::string& host, int& port,
                    std::string& path, bool& is_https) {
    is_https = false;
    std::string lower;
    lower.reserve(url.size());
    for (char c : url) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    std::string_view rest;
    if (lower.rfind("https://", 0) == 0) {
        rest = url.substr(8);
        is_https = true;
        port = 443;
    } else if (lower.rfind("http://", 0) == 0) {
        rest = url.substr(7);
        port = 80;
    } else {
        return false;
    }
    auto slash = rest.find('/');
    std::string_view host_port =
        slash == std::string_view::npos ? rest : rest.substr(0, slash);
    path = slash == std::string_view::npos
               ? std::string("/")
               : std::string(rest.substr(slash));
    auto colon = host_port.find(':');
    if (colon == std::string_view::npos) {
        host = std::string(host_port);
    } else {
        host = std::string(host_port.substr(0, colon));
        try {
            port = std::stoi(std::string(host_port.substr(colon + 1)));
        } catch (...) {
            return false;
        }
    }
    return !host.empty();
}

// Minimal HTTP/1.1 GET used only for CDP endpoint discovery.  Returns the
// response body stripped of headers (empty on any failure).  Does NOT
// follow redirects or support HTTPS — the CDP debug endpoint is HTTP only.
std::string http_discovery_get(const std::string& url, int timeout_sec) {
#if defined(_WIN32)
    (void)url;
    (void)timeout_sec;
    return {};
#else
    std::string host;
    int port = 0;
    std::string path;
    bool is_https = false;
    if (!parse_http_url(url, host, port, path, is_https)) return {};
    if (is_https) return {};  // TLS not supported in this minimal client

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                      &res) != 0 ||
        !res) {
        return {};
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return {};
    }
    struct timeval tv {};
    tv.tv_sec = timeout_sec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::freeaddrinfo(res);
        ::close(fd);
        return {};
    }
    ::freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                      std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    if (::write(fd, req.data(), req.size()) !=
        static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return {};
    }
    std::string response;
    char buf[4096];
    while (true) {
        auto n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return {};
    return response.substr(hdr_end + 4);
#endif
}

// Static override pointer — tests can install a canned body for a given
// discovery URL to avoid real sockets.  Guarded by a mutex because tools
// tests run in parallel processes but the static is per-test anyway.
struct DiscoveryOverride {
    std::string url;
    std::string body;
};
DiscoveryOverride& override_slot() {
    static DiscoveryOverride slot;
    return slot;
}
std::mutex& override_mu() {
    static std::mutex m;
    return m;
}

}  // namespace

// ---- Endpoint classification ----------------------------------------------

ResolvedCdpEndpoint classify_browser_cdp_endpoint(std::string_view raw) {
    ResolvedCdpEndpoint out;
    auto trimmed = trim_str(raw);
    if (trimmed.empty()) {
        out.kind = CdpEndpointKind::Empty;
        return out;
    }
    if (starts_with_ci(trimmed, "wss://")) {
        out.kind = CdpEndpointKind::Unsupported;
        out.error =
            "wss:// CDP endpoints are not yet supported in this backend.";
        return out;
    }
    // Direct ws:// URL with a session path — use as-is.
    auto lower_pos = trimmed.find("/devtools/");
    if (starts_with_ci(trimmed, "ws://") &&
        lower_pos != std::string::npos) {
        out.kind = CdpEndpointKind::WebSocketDirect;
        out.ws_url = trimmed;
        return out;
    }
    // Otherwise reuse the shared classifier to compute the discovery URL.
    auto cls = browser::classify_cdp_url(trimmed);
    switch (cls.kind) {
        case browser::CdpUrlKind::WebsocketDirect:
            out.kind = CdpEndpointKind::WebSocketDirect;
            out.ws_url = cls.raw;
            return out;
        case browser::CdpUrlKind::WebsocketBare:
        case browser::CdpUrlKind::HttpVersion:
        case browser::CdpUrlKind::HttpRoot:
            out.kind = CdpEndpointKind::NeedsDiscovery;
            out.discovery_url = cls.version_url;
            return out;
        case browser::CdpUrlKind::Empty:
            out.kind = CdpEndpointKind::Empty;
            return out;
        case browser::CdpUrlKind::Unknown:
            out.kind = CdpEndpointKind::Invalid;
            out.error = "CDP endpoint not recognised: " + cls.raw;
            return out;
    }
    out.kind = CdpEndpointKind::Invalid;
    out.error = "CDP endpoint could not be classified";
    return out;
}

// ---- Env + config resolution ----------------------------------------------

std::string resolve_cdp_override() {
    if (const char* env = std::getenv("BROWSER_CDP_URL"); env && *env) {
        return trim_str(env);
    }
    // Config overlay resolution is intentionally lightweight — tools link
    // hermes::config transitively via production binaries, but the tools
    // test harness doesn't.  The env var covers both paths for P0.
    return {};
}

// ---- CDP call -------------------------------------------------------------

namespace {

// Drive the attach + dispatch protocol over an already-connected
// WsConnection.  ``call_id`` is incremented for each outbound request.
BrowserCdpResult run_call_on(WsConnection& ws, const BrowserCdpArgs& args) {
    BrowserCdpResult out;
    std::string session_id;
    auto deadline =
        std::chrono::steady_clock::now() + args.timeout;
    auto remaining = [&]() -> std::chrono::seconds {
        auto r = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (r.count() < 1) return std::chrono::seconds(1);
        return r;
    };

    if (!args.target_id.empty()) {
        int attach_id = g_cdp_call_id.fetch_add(1);
        nlohmann::json attach_req = {
            {"id", attach_id},
            {"method", "Target.attachToTarget"},
            {"params",
             {{"targetId", args.target_id}, {"flatten", true}}},
        };
        if (!ws.send_text(attach_req.dump())) {
            out.error = "WS send failed during attachToTarget: " + ws.error();
            return out;
        }
        while (true) {
            std::string frame;
            if (!ws.recv_text(frame, remaining())) {
                out.error =
                    "Timed out attaching to target: " + ws.error();
                return out;
            }
            nlohmann::json msg;
            try {
                msg = nlohmann::json::parse(frame);
            } catch (...) {
                out.error = "attachToTarget: non-JSON response";
                return out;
            }
            if (!msg.contains("id") ||
                msg["id"].get<int>() != attach_id) {
                continue;  // event/other response — skip
            }
            if (msg.contains("error")) {
                out.error = "Target.attachToTarget failed: " +
                            msg["error"].dump();
                return out;
            }
            auto sid = msg.value("/result/sessionId"_json_pointer,
                                  nlohmann::json());
            if (!sid.is_string() || sid.get<std::string>().empty()) {
                out.error =
                    "Target.attachToTarget did not return a sessionId";
                return out;
            }
            session_id = sid.get<std::string>();
            break;
        }
    }

    int call_id = g_cdp_call_id.fetch_add(1);
    nlohmann::json req = {
        {"id", call_id},
        {"method", args.method},
        {"params", args.params.is_object() ? args.params
                                            : nlohmann::json::object()},
    };
    if (!session_id.empty()) req["sessionId"] = session_id;
    if (!ws.send_text(req.dump())) {
        out.error = "WS send failed: " + ws.error();
        return out;
    }

    while (true) {
        std::string frame;
        if (!ws.recv_text(frame, remaining())) {
            out.error = "Timed out waiting for response to " + args.method +
                        ": " + ws.error();
            return out;
        }
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(frame);
        } catch (...) {
            out.error = "non-JSON CDP response";
            return out;
        }
        if (!msg.contains("id") || msg["id"].get<int>() != call_id) {
            continue;  // stream event — skip
        }
        if (msg.contains("error")) {
            out.error = "CDP error: " + msg["error"].dump();
            return out;
        }
        out.success = true;
        out.result = msg.value("result", nlohmann::json::object());
        return out;
    }
}

}  // namespace

BrowserCdpResult cdp_call(const std::string& ws_url,
                          const BrowserCdpArgs& args) {
    BrowserCdpResult out;
    if (args.method.empty()) {
        out.error = "method is required";
        return out;
    }
    if (ws_url.empty() || !starts_with_ci(ws_url, "ws://")) {
        out.error = "ws:// URL required; got: " + ws_url;
        return out;
    }
    WsConnection ws;
    if (!ws.connect(ws_url, args.timeout)) {
        out.error = "failed to connect to CDP endpoint: " + ws.error();
        return out;
    }
    return run_call_on(ws, args);
}

// ---- Registry handler -----------------------------------------------------

namespace {

// Resolve the final ws:// URL for a call, performing discovery via HTTP
// when necessary.  Uses the injected override (for tests) when the
// endpoint matches, else falls back to a real HTTP GET.
ResolvedCdpEndpoint resolve_for_call(std::string_view raw) {
    auto classified = classify_browser_cdp_endpoint(raw);
    if (classified.kind != CdpEndpointKind::NeedsDiscovery) {
        return classified;
    }
    std::string body;
    {
        std::lock_guard<std::mutex> lk(override_mu());
        auto& slot = override_slot();
        if (!slot.url.empty() && slot.url == classified.discovery_url) {
            body = slot.body;
        }
    }
    if (body.empty()) {
        body = http_discovery_get(classified.discovery_url, 5);
    }
    if (body.empty()) {
        classified.kind = CdpEndpointKind::Invalid;
        classified.error = "CDP discovery endpoint unreachable: " +
                           classified.discovery_url;
        return classified;
    }
    auto ws = browser::extract_ws_from_version_json(body);
    if (ws.empty()) {
        classified.kind = CdpEndpointKind::Invalid;
        classified.error =
            "CDP discovery response missing webSocketDebuggerUrl";
        return classified;
    }
    classified.kind = CdpEndpointKind::WebSocketDirect;
    classified.ws_url = ws;
    return classified;
}

std::string handle_browser_cdp(const nlohmann::json& args,
                               const ToolContext& /*ctx*/) {
    std::string method =
        args.value("method", std::string());
    if (method.empty()) {
        return tool_error(
            "'method' is required (e.g. 'Target.getTargets')",
            {{"cdp_docs", std::string(kCdpDocsUrl)}});
    }

    auto endpoint_raw = resolve_cdp_override();
    if (endpoint_raw.empty()) {
        return tool_error(
            "No CDP endpoint is available. Run '/browser connect' to "
            "attach to a running Chrome, or set 'browser.cdp_url' in "
            "config.yaml.",
            {{"cdp_docs", std::string(kCdpDocsUrl)}});
    }

    auto resolved = resolve_for_call(endpoint_raw);
    if (resolved.kind != CdpEndpointKind::WebSocketDirect) {
        std::string msg = resolved.error.empty()
                              ? "CDP endpoint could not be resolved to a "
                                "WebSocket URL"
                              : resolved.error;
        return tool_error(msg, {{"method", method}});
    }

    BrowserCdpArgs cargs;
    cargs.method = method;
    if (args.contains("params") && args["params"].is_object()) {
        cargs.params = args["params"];
    } else if (args.contains("params") && !args["params"].is_null()) {
        return tool_error(
            "'params' must be an object", {{"method", method}});
    }
    if (args.contains("target_id") && args["target_id"].is_string()) {
        cargs.target_id = args["target_id"].get<std::string>();
    } else if (args.contains("sessionId") &&
               args["sessionId"].is_string()) {
        // Mirror Python: accept ``sessionId`` as an alias for compat.
        cargs.target_id = args["sessionId"].get<std::string>();
    }
    double tmo = args.value("timeout", 30.0);
    if (tmo < 1.0) tmo = 30.0;
    if (tmo > 300.0) tmo = 300.0;
    cargs.timeout = std::chrono::seconds(static_cast<int>(tmo));

    auto r = cdp_call(resolved.ws_url, cargs);
    if (!r.success) {
        return tool_error(r.error, {{"method", method}});
    }
    nlohmann::json payload = {
        {"success", true},
        {"method", method},
        {"result", r.result},
    };
    if (!cargs.target_id.empty()) payload["target_id"] = cargs.target_id;
    return tool_result(payload);
}

bool browser_cdp_check() {
    return !resolve_cdp_override().empty();
}

}  // namespace

// ---- Test seam (not in header) --------------------------------------------
//
// Declared as ``extern`` for tests; the internals are file-local so other
// source files stay decoupled.
namespace browser_cdp_test_support {

void set_discovery_override(const std::string& discovery_url,
                            const std::string& body) {
    std::lock_guard<std::mutex> lk(override_mu());
    override_slot().url = discovery_url;
    override_slot().body = body;
}

void clear_discovery_override() {
    std::lock_guard<std::mutex> lk(override_mu());
    override_slot().url.clear();
    override_slot().body.clear();
}

}  // namespace browser_cdp_test_support

void register_browser_cdp_tool() {
    auto& reg = ToolRegistry::instance();
    ToolEntry e;
    e.name = "browser_cdp";
    e.toolset = "browser";
    e.description =
        "Send a raw Chrome DevTools Protocol (CDP) command.  Escape "
        "hatch for browser operations not covered by the regular "
        "browser_* tools.  Requires a reachable CDP endpoint (set via "
        "/browser connect or the browser.cdp_url config key).  Method "
        "reference: https://chromedevtools.github.io/devtools-protocol/";
    e.schema = {
        {"type", "object"},
        {"properties",
         {
             {"method",
              {{"type", "string"},
               {"description",
                "CDP method name, e.g. 'Target.getTargets', "
                "'Runtime.evaluate', 'Page.handleJavaScriptDialog'."}}},
             {"params",
              {{"type", "object"},
               {"additionalProperties", true},
               {"description",
                "Method-specific parameters as a JSON object.  Omit or "
                "pass {} for methods that take no parameters."}}},
             {"target_id",
              {{"type", "string"},
               {"description",
                "Optional.  Target/tab id from Target.getTargets.  "
                "Required for page-level methods; must be omitted for "
                "browser-level methods (Target.*, Browser.*, Storage.*)."}}},
             {"timeout",
              {{"type", "number"},
               {"default", 30},
               {"description",
                "Timeout in seconds (default 30, max 300)."}}},
         }},
        {"required", nlohmann::json::array({"method"})},
    };
    e.handler = handle_browser_cdp;
    e.check_fn = browser_cdp_check;
    e.requires_env = {"BROWSER_CDP_URL"};
    e.emoji = "\xF0\x9F\xA7\xAA";  // test-tube — same as Python
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
