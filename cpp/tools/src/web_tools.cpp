#include "hermes/tools/web_tools.hpp"
#include "hermes/tools/registry.hpp"

#include <cstdlib>
#include <string>

namespace hermes::tools {

namespace {

hermes::llm::HttpTransport* g_web_transport = nullptr;

std::string handle_web_search(const nlohmann::json& args,
                              const ToolContext& /*ctx*/) {
    if (!g_web_transport) {
        return tool_error(
            "HTTP transport not available — rebuild with cpr");
    }

    const auto query = args.at("query").get<std::string>();
    const int num_results =
        args.contains("num_results") ? args["num_results"].get<int>() : 5;

    const char* api_key = std::getenv("EXA_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return tool_error("EXA_API_KEY not set");
    }

    nlohmann::json req_body;
    req_body["query"] = query;
    req_body["numResults"] = num_results;

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["x-api-key"] = api_key;

    auto resp = g_web_transport->post_json(
        "https://api.exa.ai/search", headers, req_body.dump());

    if (resp.status_code != 200) {
        return tool_error("Exa API error",
                          {{"status", resp.status_code},
                           {"body", resp.body}});
    }

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("results")) {
        return tool_error("malformed response from Exa API");
    }

    nlohmann::json out_results = nlohmann::json::array();
    for (const auto& r : body["results"]) {
        nlohmann::json item;
        item["title"] = r.value("title", "");
        item["url"] = r.value("url", "");
        item["snippet"] = r.value("text", "");
        out_results.push_back(std::move(item));
    }

    nlohmann::json result;
    result["results"] = std::move(out_results);
    return tool_result(result);
}

std::string handle_web_extract(const nlohmann::json& args,
                               const ToolContext& /*ctx*/) {
    if (!g_web_transport) {
        return tool_error(
            "HTTP transport not available — rebuild with cpr");
    }

    const auto url = args.at("url").get<std::string>();
    const int max_length =
        args.contains("max_length") ? args["max_length"].get<int>() : 10000;

    const char* api_key = std::getenv("FIRECRAWL_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return tool_error("FIRECRAWL_API_KEY not set");
    }

    nlohmann::json req_body;
    req_body["url"] = url;
    req_body["maxLength"] = max_length;

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = std::string("Bearer ") + api_key;

    auto resp = g_web_transport->post_json(
        "https://api.firecrawl.dev/v1/scrape", headers, req_body.dump());

    if (resp.status_code != 200) {
        return tool_error("Firecrawl API error",
                          {{"status", resp.status_code},
                           {"body", resp.body}});
    }

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("data")) {
        return tool_error("malformed response from Firecrawl API");
    }

    const auto& data = body["data"];
    nlohmann::json result;
    result["content"] = data.value("markdown", data.value("content", ""));
    result["title"] = data.value("title", "");
    result["url"] = url;
    return tool_result(result);
}

}  // namespace

void register_web_tools(hermes::llm::HttpTransport* transport) {
    g_web_transport = transport;
    auto& reg = ToolRegistry::instance();

    {
        ToolEntry e;
        e.name = "web_search";
        e.toolset = "web";
        e.description = "Search the web via Exa API";
        e.emoji = "\xF0\x9F\x94\x8D";  // magnifying glass
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}, {"description", "Search query"}}},
              {"num_results",
               {{"type", "integer"},
                {"description", "Number of results (default 5)"}}}}},
            {"required", nlohmann::json::array({"query"})}};
        e.handler = handle_web_search;
        e.check_fn = [] {
            const char* k = std::getenv("EXA_API_KEY");
            return k && k[0] != '\0';
        };
        reg.register_tool(std::move(e));
    }

    {
        ToolEntry e;
        e.name = "web_extract";
        e.toolset = "web";
        e.description = "Extract content from a URL via Firecrawl API";
        e.emoji = "\xF0\x9F\x93\x84";  // page
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "URL to scrape"}}},
              {"max_length",
               {{"type", "integer"},
                {"description",
                 "Max content length in chars (default 10000)"}}}}},
            {"required", nlohmann::json::array({"url"})}};
        e.handler = handle_web_extract;
        e.check_fn = [] {
            const char* k = std::getenv("FIRECRAWL_API_KEY");
            return k && k[0] != '\0';
        };
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
