#include "hermes/tools/browser_tool.hpp"
#include "hermes/tools/browser_backend.hpp"
#include "hermes/tools/registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::tools {

namespace {

std::string to_lower_str(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string trim_str(std::string_view s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

// Decode percent-encoded URL bytes — minimal version sufficient for the
// secret-marker scan.  Unknown escapes are emitted verbatim.
std::string url_decode(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (c == '%' && i + 2 < src.size()) {
            int hi = from_hex(src[i + 1]);
            int lo = from_hex(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string extract_host(std::string_view url) {
    auto pos = url.find("://");
    std::string_view rest = pos == std::string_view::npos ? url : url.substr(pos + 3);
    auto end = rest.find_first_of("/?#");
    if (end != std::string_view::npos) rest = rest.substr(0, end);
    auto at = rest.find('@');
    if (at != std::string_view::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') {
        // IPv6 literal — strip brackets, drop optional :port.
        auto close = rest.find(']');
        if (close != std::string_view::npos) {
            return std::string(rest.substr(1, close - 1));
        }
    }
    auto colon = rest.find_last_of(':');
    if (colon != std::string_view::npos) {
        // Only treat as port when the suffix is all digits.
        bool all_digits = true;
        for (auto c : rest.substr(colon + 1)) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits && colon + 1 < rest.size()) {
            rest = rest.substr(0, colon);
        }
    }
    return std::string(rest);
}

bool is_ipv4_private(std::string_view host) {
    // Quick parse — accept four numeric octets separated by '.'.
    int parts[4] = {-1, -1, -1, -1};
    int idx = 0;
    int cur = 0;
    bool seen = false;
    for (char c : host) {
        if (c == '.') {
            if (!seen || cur > 255) return false;
            parts[idx++] = cur;
            cur = 0;
            seen = false;
            if (idx > 3) return false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur = cur * 10 + (c - '0');
            seen = true;
            if (cur > 255) return false;
        } else {
            return false;
        }
    }
    if (idx != 3 || !seen) return false;
    parts[3] = cur;
    if (parts[0] == 10) return true;
    if (parts[0] == 127) return true;
    if (parts[0] == 0) return true;
    if (parts[0] == 169 && parts[1] == 254) return true;  // link-local + metadata
    if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) return true;
    if (parts[0] == 192 && parts[1] == 168) return true;
    if (parts[0] >= 224) return true;  // multicast / reserved
    return false;
}

bool is_ipv6_private(std::string_view host) {
    auto lower = to_lower_str(host);
    if (lower == "::1") return true;
    if (lower == "::") return true;
    if (lower.compare(0, 2, "fe") == 0 && lower.size() >= 4) {
        char third = lower[2];
        // fe80::/10 link-local, fec0::/10 deprecated site-local
        if (third == '8' || third == '9' || third == 'a' || third == 'b' ||
            third == 'c' || third == 'd' || third == 'e' || third == 'f') {
            return true;
        }
    }
    if (lower.compare(0, 2, "fc") == 0) return true;
    if (lower.compare(0, 2, "fd") == 0) return true;
    return false;
}

std::string handle_browser_navigate(const nlohmann::json& args,
                                    const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto url = args.at("url").get<std::string>();
    b->navigate(url);
    nlohmann::json r;
    r["navigated"] = true;
    r["url"] = url;
    return tool_result(r);
}

std::string handle_browser_snapshot(const nlohmann::json& /*args*/,
                                    const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto snap = b->snapshot();
    nlohmann::json r;
    r["url"] = snap.url;
    r["title"] = snap.title;
    r["elements"] = nlohmann::json::array();
    for (const auto& el : snap.elements) {
        r["elements"].push_back({
            {"ref", el.ref}, {"tag", el.tag},
            {"text", el.text}, {"role", el.role}
        });
    }
    return tool_result(r);
}

std::string handle_browser_click(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto ref = args.at("ref").get<std::string>();
    bool dbl = args.contains("dbl_click") && args["dbl_click"].get<bool>();
    b->click(ref, dbl);
    return tool_result({{"clicked", true}});
}

std::string handle_browser_type(const nlohmann::json& args,
                                const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto ref = args.at("ref").get<std::string>();
    auto text = args.at("text").get<std::string>();
    bool submit = args.contains("submit") && args["submit"].get<bool>();
    b->type(ref, text, submit);
    return tool_result({{"typed", true}});
}

std::string handle_browser_scroll(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto dir = args.at("direction").get<std::string>();
    b->scroll(dir);
    return tool_result({{"scrolled", true}});
}

std::string handle_browser_back(const nlohmann::json& /*args*/,
                                const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    b->go_back();
    return tool_result({{"navigated_back", true}});
}

std::string handle_browser_press(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto key = args.at("key").get<std::string>();
    b->press_key(key);
    return tool_result({{"pressed", true}});
}

std::string handle_browser_get_images(const nlohmann::json& /*args*/,
                                      const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto urls = b->get_image_urls();
    nlohmann::json r;
    r["images"] = urls;
    return tool_result(r);
}

std::string handle_browser_vision(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    // prompt is accepted but not used until vision LLM is wired.
    (void)args.at("prompt").get<std::string>();
    auto shot = b->screenshot_base64();
    nlohmann::json r;
    r["analysis"] = "vision not wired";
    r["screenshot_size"] = static_cast<int>(shot.size());
    return tool_result(r);
}

std::string handle_browser_console(const nlohmann::json& args,
                                   const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto expr = args.at("expression").get<std::string>();
    auto cr = b->evaluate_js(expr);
    nlohmann::json r;
    r["value"] = cr.value;
    r["is_error"] = cr.is_error;
    return tool_result(r);
}

}  // namespace

// ---- URL safety helpers --------------------------------------------------

bool url_contains_secret_marker(std::string_view url) {
    static const std::array<std::string, 6> markers = {
        "sk-ant-", "sk-or-", "sk-proj-", "sk_live_", "AKIA", "ghp_"};
    auto check = [&](std::string_view s) {
        std::string lower = to_lower_str(s);
        for (const auto& m : markers) {
            std::string lm = to_lower_str(m);
            if (lower.find(lm) != std::string::npos) return true;
        }
        return false;
    };
    if (check(url)) return true;
    auto decoded = url_decode(url);
    return check(decoded);
}

bool url_targets_private_address(std::string_view url) {
    auto host = extract_host(url);
    if (host.empty()) return false;
    auto lower = to_lower_str(host);
    if (lower == "localhost") return true;
    if (lower.size() >= 6 &&
        lower.compare(lower.size() - 6, 6, ".local") == 0) {
        return true;
    }
    if (is_ipv4_private(host)) return true;
    if (host.find(':') != std::string::npos && is_ipv6_private(host)) return true;
    return false;
}

bool is_safe_browser_url(std::string_view url) {
    if (url.empty()) return false;
    if (url_contains_secret_marker(url)) return false;
    if (url_targets_private_address(url)) return false;
    return true;
}

// ---- Bot-detection heuristics -------------------------------------------

const std::vector<std::string>& bot_detection_title_patterns() {
    static const std::vector<std::string> v = {
        "access denied",
        "access to this page has been denied",
        "blocked",
        "bot detected",
        "verification required",
        "please verify",
        "are you a robot",
        "captcha",
        "cloudflare",
        "ddos protection",
        "checking your browser",
        "just a moment",
        "attention required",
    };
    return v;
}

bool looks_like_bot_detection(std::string_view title) {
    if (title.empty()) return false;
    auto lower = to_lower_str(title);
    for (const auto& p : bot_detection_title_patterns()) {
        if (lower.find(p) != std::string::npos) return true;
    }
    return false;
}

// ---- Snapshot helpers ---------------------------------------------------

std::string truncate_snapshot(std::string_view text, std::size_t max_chars) {
    if (text.size() <= max_chars) return std::string(text);
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) lines.push_back(text.substr(start));

    std::string acc;
    std::size_t kept = 0;
    std::size_t budget = max_chars > 80 ? max_chars - 80 : max_chars;
    for (const auto& line : lines) {
        if (acc.size() + line.size() + 1 > budget) break;
        if (kept) acc.push_back('\n');
        acc.append(line.data(), line.size());
        ++kept;
    }
    auto remaining = lines.size() - kept;
    if (remaining > 0) {
        acc.append("\n[... ");
        acc.append(std::to_string(remaining));
        acc.append(
            " more lines truncated, use browser_snapshot for full content]");
    }
    return acc;
}

std::optional<std::string> extract_screenshot_path(std::string_view body) {
    if (body.empty()) return std::nullopt;
    static const std::array<std::regex, 3> patterns = {
        std::regex(R"(Screenshot saved to ['\"](?:(/[^'\"]+?\.png))['\"])"),
        std::regex(R"(Screenshot saved to (/\S+?\.png)(?:\s|$))"),
        std::regex(R"((/\S+?\.png)(?:\s|$))"),
    };
    std::string text(body);
    for (const auto& re : patterns) {
        std::smatch m;
        if (std::regex_search(text, m, re)) {
            std::string path = m[1].str();
            if (!path.empty()) return path;
        }
    }
    return std::nullopt;
}

std::string normalise_cdp_endpoint(std::string_view raw) {
    auto trimmed = trim_str(raw);
    if (trimmed.empty()) return {};
    auto lower = to_lower_str(trimmed);
    if (lower.find("/devtools/browser/") != std::string::npos) return trimmed;
    // Drop trailing slashes (except after the scheme separator).
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();

    // Convert bare ws://host:port to http(s)://host:port for the discovery
    // probe.  Anything richer is left alone.
    auto starts_with = [&](std::string_view prefix) {
        return lower.size() >= prefix.size() &&
               lower.compare(0, prefix.size(), prefix) == 0;
    };
    if (starts_with("ws://") || starts_with("wss://")) {
        // Has a path beyond the host? Then leave it alone.
        std::string body = trimmed.substr(trimmed.find("://") + 3);
        if (body.find('/') != std::string::npos) return trimmed;
        return (starts_with("ws://") ? std::string("http://")
                                     : std::string("https://")) +
               body;
    }
    return trimmed;
}

std::string normalise_browser_ref(std::string_view raw) {
    auto trimmed = trim_str(raw);
    if (trimmed.empty()) return {};
    static const std::regex re(R"(\[\s*ref\s*=\s*([A-Za-z0-9_-]+)\s*\])");
    std::smatch m;
    std::string copy = trimmed;
    if (std::regex_search(copy, m, re)) {
        return m[1].str();
    }
    return trimmed;
}

bool is_known_browser_key(std::string_view key) {
    static const std::unordered_set<std::string> known = {
        "Enter",      "Escape",    "Tab",      "Space",   "Backspace",
        "Delete",     "ArrowUp",   "ArrowDown", "ArrowLeft",
        "ArrowRight", "Home",      "End",      "PageUp",  "PageDown",
        "F1",         "F2",        "F3",       "F4",      "F5",
        "F6",         "F7",        "F8",       "F9",      "F10",
        "F11",        "F12",       "Insert",   "Shift",   "Control",
        "Alt",        "Meta"};
    auto trimmed = trim_str(key);
    if (trimmed.empty()) return false;
    auto chord = parse_browser_key_chord(trimmed);
    if (chord.key.empty()) return false;
    if (known.count(chord.key)) return true;
    return chord.key.size() == 1;  // single-char keys are always accepted
}

BrowserKeyChord parse_browser_key_chord(std::string_view raw) {
    BrowserKeyChord out;
    auto trimmed = trim_str(raw);
    if (trimmed.empty()) return out;

    std::vector<std::string> parts;
    std::stringstream ss(trimmed);
    std::string item;
    while (std::getline(ss, item, '+')) {
        auto t = trim_str(item);
        if (!t.empty()) parts.push_back(t);
    }
    if (parts.empty()) return out;
    out.key = parts.back();
    parts.pop_back();
    for (auto& p : parts) {
        out.modifiers.push_back(to_lower_str(p));
    }
    return out;
}

void register_browser_tools() {
    auto& reg = ToolRegistry::instance();
    auto browser_check = [] { return get_browser_backend() != nullptr; };

    // 1. browser_navigate
    {
        ToolEntry e;
        e.name = "browser_navigate";
        e.toolset = "browser";
        e.description = "Navigate browser to a URL";
        e.schema = {
            {"type", "object"},
            {"properties", {{"url", {{"type", "string"}, {"description", "URL to navigate to"}}}}},
            {"required", nlohmann::json::array({"url"})}};
        e.handler = handle_browser_navigate;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 2. browser_snapshot
    {
        ToolEntry e;
        e.name = "browser_snapshot";
        e.toolset = "browser";
        e.description = "Get a snapshot of the current page DOM";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_snapshot;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 3. browser_click
    {
        ToolEntry e;
        e.name = "browser_click";
        e.toolset = "browser";
        e.description = "Click an element by ref";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"ref", {{"type", "string"}, {"description", "Element reference"}}},
                {"dbl_click", {{"type", "boolean"}, {"description", "Double-click (default false)"}}}}},
            {"required", nlohmann::json::array({"ref"})}};
        e.handler = handle_browser_click;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 4. browser_type
    {
        ToolEntry e;
        e.name = "browser_type";
        e.toolset = "browser";
        e.description = "Type text into an element";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"ref", {{"type", "string"}, {"description", "Element reference"}}},
                {"text", {{"type", "string"}, {"description", "Text to type"}}},
                {"submit", {{"type", "boolean"}, {"description", "Submit after typing (default false)"}}}}},
            {"required", nlohmann::json::array({"ref", "text"})}};
        e.handler = handle_browser_type;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 5. browser_scroll
    {
        ToolEntry e;
        e.name = "browser_scroll";
        e.toolset = "browser";
        e.description = "Scroll the page in a direction";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"direction", {{"type", "string"}, {"description", "up|down|left|right"}}}}},
            {"required", nlohmann::json::array({"direction"})}};
        e.handler = handle_browser_scroll;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 6. browser_back
    {
        ToolEntry e;
        e.name = "browser_back";
        e.toolset = "browser";
        e.description = "Navigate back in browser history";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_back;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 7. browser_press
    {
        ToolEntry e;
        e.name = "browser_press";
        e.toolset = "browser";
        e.description = "Press a keyboard key";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}, {"description", "Key to press, e.g. Enter, Ctrl+A"}}}}},
            {"required", nlohmann::json::array({"key"})}};
        e.handler = handle_browser_press;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 8. browser_get_images
    {
        ToolEntry e;
        e.name = "browser_get_images";
        e.toolset = "browser";
        e.description = "Get all image URLs on the page";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_get_images;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 9. browser_vision
    {
        ToolEntry e;
        e.name = "browser_vision";
        e.toolset = "browser";
        e.description = "Take screenshot and analyze with vision LLM";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"}, {"description", "Vision analysis prompt"}}}}},
            {"required", nlohmann::json::array({"prompt"})}};
        e.handler = handle_browser_vision;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 10. browser_console
    {
        ToolEntry e;
        e.name = "browser_console";
        e.toolset = "browser";
        e.description = "Evaluate JavaScript in the browser console";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"expression", {{"type", "string"}, {"description", "JavaScript expression to evaluate"}}}}},
            {"required", nlohmann::json::array({"expression"})}};
        e.handler = handle_browser_console;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
