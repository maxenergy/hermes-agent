// Implementation of hermes/tools/browser_tool_depth.hpp.
#include "hermes/tools/browser_tool_depth.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>

namespace hermes::tools::browser_depth {

namespace {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---- Command timeout --------------------------------------------------

int resolve_command_timeout(std::optional<int> configured, int default_value) {
    if (!configured.has_value()) {
        return default_value;
    }
    int value = *configured;
    if (value <= 0) {
        return default_value;
    }
    return std::max(value, kBrowserCommandTimeoutFloor);
}

// ---- CDP endpoint resolution ------------------------------------------

CdpStrategy classify_cdp_endpoint(std::string_view raw) {
    const std::string t = trim(raw);
    if (t.empty()) {
        return CdpStrategy::Empty;
    }
    const std::string lower = to_lower(t);
    if (lower.find("/devtools/browser/") != std::string::npos) {
        return CdpStrategy::PassThroughWebsocket;
    }
    if (starts_with(lower, "ws://") || starts_with(lower, "wss://")) {
        // A bare ws://host:port is exactly two colons (scheme colon + host
        // colon) and the last ``:port`` segment is numeric with no path
        // segment afterwards.
        const int colons = static_cast<int>(std::count(t.begin(), t.end(), ':'));
        auto scheme_end = t.find("://");
        const std::string after_scheme =
            scheme_end == std::string::npos ? t : t.substr(scheme_end + 3);
        const auto slash = after_scheme.find('/');
        const bool has_no_path =
            slash == std::string::npos ||
            slash + 1 == after_scheme.size();  // trailing / only
        const auto last_colon = after_scheme.rfind(':');
        bool port_numeric = false;
        if (last_colon != std::string::npos) {
            const std::string port = after_scheme.substr(last_colon + 1);
            std::string port_trim = port;
            while (!port_trim.empty() && port_trim.back() == '/') {
                port_trim.pop_back();
            }
            port_numeric = !port_trim.empty() &&
                           std::all_of(port_trim.begin(), port_trim.end(),
                                       [](unsigned char c) {
                                           return std::isdigit(c) != 0;
                                       });
        }
        if (colons == 2 && has_no_path && port_numeric) {
            return CdpStrategy::WebsocketDiscovery;
        }
        return CdpStrategy::PassThroughWebsocket;
    }
    return CdpStrategy::HttpDiscovery;
}

std::string build_cdp_discovery_url(std::string_view http_or_ws_url) {
    std::string s = trim(http_or_ws_url);
    if (s.empty()) {
        return s;
    }
    const std::string lower = to_lower(s);

    // Upgrade ws:// / wss:// to http:// / https:// for the discovery
    // probe.
    if (starts_with(lower, "ws://")) {
        s = "http://" + s.substr(5);
    } else if (starts_with(lower, "wss://")) {
        s = "https://" + s.substr(6);
    }

    if (ends_with(to_lower(s), "/json/version")) {
        return s;
    }
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    s += "/json/version";
    return s;
}

std::string pick_websocket_from_discovery(std::string_view raw_endpoint,
                                          std::string_view response_body) {
    try {
        auto body = nlohmann::json::parse(response_body);
        if (body.is_object() && body.contains("webSocketDebuggerUrl")) {
            const auto& v = body.at("webSocketDebuggerUrl");
            if (v.is_string()) {
                const std::string s = trim(v.get<std::string>());
                if (!s.empty()) {
                    return s;
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        // fall through
    }
    return std::string{raw_endpoint};
}

// ---- Provider registry -------------------------------------------------

const std::vector<std::string>& supported_cloud_providers() {
    static const std::vector<std::string> providers = {
        "browserbase", "browser-use", "firecrawl",
    };
    return providers;
}

bool is_known_cloud_provider(std::string_view name) {
    for (const auto& p : supported_cloud_providers()) {
        if (p == name) {
            return true;
        }
    }
    return false;
}

std::string normalise_cloud_provider_name(std::string_view raw) {
    const std::string lower = to_lower(trim(raw));
    if (lower.empty()) {
        return "";
    }
    if (lower == "browseruse" || lower == "browser_use") {
        return "browser-use";
    }
    if (lower == "bb") {
        return "browserbase";
    }
    if (lower == "none" || lower == "off" || lower == "disabled") {
        return "local";
    }
    if (lower == "local") {
        return "local";
    }
    if (is_known_cloud_provider(lower)) {
        return lower;
    }
    return "";
}

// ---- Install hints -----------------------------------------------------

std::string browser_install_hint(bool is_termux) {
    if (is_termux) {
        return "npm install -g agent-browser && agent-browser install";
    }
    return "npm install -g agent-browser && agent-browser install --with-deps";
}

std::string termux_browser_install_error() {
    return std::string{
        "Local browser automation on Termux cannot rely on the bare npx "
        "fallback. Install agent-browser explicitly first: "} +
           browser_install_hint(true);
}

bool requires_real_termux_browser_install(std::string_view browser_cmd,
                                          bool is_termux, bool is_local_mode) {
    if (!is_termux || !is_local_mode) {
        return false;
    }
    return trim(browser_cmd) == "npx agent-browser";
}

// ---- SSRF toggle -------------------------------------------------------

bool ssrf_protection_applies(bool is_camofox_mode, bool has_cloud_provider) {
    // _is_local_backend() is true when camofox OR no cloud provider.
    // SSRF only meaningful for non-local backends.
    const bool local_backend = is_camofox_mode || !has_cloud_provider;
    return !local_backend;
}

bool resolve_allow_private_urls(std::optional<bool> configured) {
    if (!configured.has_value()) {
        return false;
    }
    return *configured;
}

// ---- Socket-path selection --------------------------------------------

std::string resolve_browser_tmpdir(std::string_view platform,
                                   std::string_view tmpdir_fallback) {
    if (platform == "darwin") {
        return "/tmp";
    }
    return std::string{tmpdir_fallback};
}

// ---- Screenshot cleanup throttle --------------------------------------

bool screenshot_cleanup_due(double last_cleanup_sec, double now_sec,
                            int interval_seconds) {
    if (last_cleanup_sec <= 0.0) {
        return true;
    }
    return (now_sec - last_cleanup_sec) >= static_cast<double>(interval_seconds);
}

// ---- Snapshot extraction prompts --------------------------------------

std::string build_snapshot_extraction_prompt(std::string_view snapshot_text,
                                             std::string_view user_task) {
    std::ostringstream oss;
    const std::string task = trim(user_task);
    if (!task.empty()) {
        oss << "You are a content extractor for a browser automation agent."
               "\n\n"
            << "The user's task is: " << task << "\n\n"
            << "Given the following page snapshot (accessibility tree "
               "representation), extract and summarize the most relevant "
               "information for completing this task. Focus on:\n"
            << "1. Interactive elements (buttons, links, inputs) that might "
               "be needed\n"
            << "2. Text content relevant to the task (prices, descriptions, "
               "headings, important info)\n"
            << "3. Navigation structure if relevant\n\n"
            << "Keep ref IDs (like [ref=e5]) for interactive elements so the "
               "agent can use them.\n\n"
            << "Page Snapshot:\n"
            << std::string{snapshot_text} << "\n\n"
            << "Provide a concise summary that preserves actionable "
               "information and relevant content.";
    } else {
        oss << "Summarize this page snapshot, preserving:\n"
            << "1. All interactive elements with their ref IDs (like "
               "[ref=e5])\n"
            << "2. Key text content and headings\n"
            << "3. Important information visible on the page\n\n"
            << "Page Snapshot:\n"
            << std::string{snapshot_text} << "\n\n"
            << "Provide a concise summary focused on interactive elements "
               "and key content.";
    }
    return oss.str();
}

// ---- "Empty OK" command allowlist -------------------------------------

const std::vector<std::string>& empty_ok_commands() {
    static const std::vector<std::string> v = {"close", "record"};
    return v;
}

bool is_empty_ok_command(std::string_view cmd) {
    for (const auto& ok : empty_ok_commands()) {
        if (ok == cmd) {
            return true;
        }
    }
    return false;
}

// ---- Homebrew node discovery ------------------------------------------

std::vector<std::string> filter_homebrew_node_dirs(
    const std::vector<std::string>& entries) {
    std::vector<std::string> out;
    for (const auto& entry : entries) {
        if (entry.size() < 4) {
            continue;
        }
        if (!starts_with(entry, "node")) {
            continue;
        }
        if (entry == "node") {
            continue;
        }
        out.push_back(entry);
    }
    return out;
}

// ---- Auxiliary-model env lookup ---------------------------------------

std::string trim_env_value(std::string_view raw) { return trim(raw); }

}  // namespace hermes::tools::browser_depth
