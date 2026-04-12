#include "hermes/tools/vision_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

namespace hermes::tools {

// ── SSRF guard ─────────────────────────────────────────────────────────

namespace {

// Very simple private-IP check on the hostname portion of a URL.
bool hostname_is_private(std::string_view host) {
    // Strip port if present.
    auto colon = host.find(':');
    if (colon != std::string_view::npos) {
        host = host.substr(0, colon);
    }

    if (host == "localhost") return true;

    // 127.x.x.x
    if (host.substr(0, 4) == "127.") return true;
    if (host == "0.0.0.0") return true;

    // 10.x.x.x
    if (host.substr(0, 3) == "10.") return true;

    // 192.168.x.x
    if (host.substr(0, 8) == "192.168.") return true;

    // 172.16-31.x.x
    if (host.substr(0, 4) == "172.") {
        auto dot2 = host.find('.', 4);
        if (dot2 != std::string_view::npos) {
            auto octet_sv = host.substr(4, dot2 - 4);
            int octet = 0;
            for (char c : octet_sv) {
                if (c < '0' || c > '9') return false;
                octet = octet * 10 + (c - '0');
            }
            if (octet >= 16 && octet <= 31) return true;
        }
    }

    // IPv6 loopback / link-local
    if (host == "::1") return true;
    if (host.substr(0, 5) == "fe80:") return true;

    return false;
}

// Extract hostname from a URL like "https://host:port/path".
std::string_view extract_host(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return {};
    auto host_start = scheme_end + 3;
    auto host_end = url.find('/', host_start);
    if (host_end == std::string_view::npos) host_end = url.size();
    return url.substr(host_start, host_end - host_start);
}

hermes::llm::HttpTransport* g_vision_transport = nullptr;

std::string handle_vision_analyze(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* transport = g_vision_transport ? g_vision_transport
                                        : hermes::llm::get_default_transport();
    // CurlTransport is always available when built with libcurl.
    assert(transport && "HTTP transport should always be available");

    const auto url = args.at("url").get<std::string>();
    const auto prompt = args.at("prompt").get<std::string>();

    // SSRF check.
    if (is_private_url(url)) {
        return tool_error("URL points to a private/loopback address");
    }

    // Download image bytes via HTTP GET.
    std::unordered_map<std::string, std::string> dl_headers;
    auto img_resp = transport->get(url, dl_headers);

    if (img_resp.status_code != 200) {
        return tool_error("Failed to download image",
                          {{"status", img_resp.status_code}});
    }

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return tool_error("OPENAI_API_KEY not set for vision model");
    }

    // Base64-encode image body (simple loop).
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    const auto& raw = img_resp.body;
    encoded.reserve(((raw.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        unsigned int n = (static_cast<unsigned char>(raw[i]) << 16);
        if (i + 1 < raw.size()) n |= (static_cast<unsigned char>(raw[i + 1]) << 8);
        if (i + 2 < raw.size()) n |= static_cast<unsigned char>(raw[i + 2]);
        encoded.push_back(b64[(n >> 18) & 0x3F]);
        encoded.push_back(b64[(n >> 12) & 0x3F]);
        encoded.push_back((i + 1 < raw.size()) ? b64[(n >> 6) & 0x3F] : '=');
        encoded.push_back((i + 2 < raw.size()) ? b64[n & 0x3F] : '=');
    }

    nlohmann::json vision_req;
    vision_req["model"] = "gpt-4o";
    vision_req["messages"] = nlohmann::json::array({
        {{"role", "user"},
         {"content",
          nlohmann::json::array(
              {{{"type", "text"}, {"text", prompt}},
               {{"type", "image_url"},
                {{"image_url",
                  {{"url", "data:image/png;base64," + encoded}}}}}})}}});

    std::unordered_map<std::string, std::string> llm_headers;
    llm_headers["Content-Type"] = "application/json";
    llm_headers["Authorization"] = std::string("Bearer ") + api_key;

    auto llm_resp = transport->post_json(
        "https://api.openai.com/v1/chat/completions", llm_headers,
        vision_req.dump());

    if (llm_resp.status_code != 200) {
        return tool_error("Vision LLM call failed",
                          {{"status", llm_resp.status_code}});
    }

    auto body = nlohmann::json::parse(llm_resp.body, nullptr, false);
    if (body.is_discarded()) {
        return tool_error("malformed vision LLM response");
    }

    std::string analysis;
    if (body.contains("choices") && !body["choices"].empty()) {
        analysis =
            body["choices"][0]["message"]["content"].get<std::string>();
    }

    nlohmann::json result;
    result["analysis"] = analysis;
    return tool_result(result);
}

}  // namespace

bool is_private_url(std::string_view url) {
    auto host = extract_host(url);
    return hostname_is_private(host);
}

void register_vision_tools(hermes::llm::HttpTransport* transport) {
    g_vision_transport = transport;
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "vision_analyze_tool";
    e.toolset = "vision";
    e.description = "Download an image and analyze it with a vision LLM";
    e.emoji = "\xF0\x9F\x91\x81";  // eye
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"url", {{"type", "string"}, {"description", "Image URL"}}},
          {"prompt",
           {{"type", "string"},
            {"description", "Analysis instruction"}}}}},
        {"required", nlohmann::json::array({"url", "prompt"})}};
    e.handler = handle_vision_analyze;
    e.check_fn = [] {
        const char* k = std::getenv("OPENAI_API_KEY");
        return k && k[0] != '\0';
    };
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
