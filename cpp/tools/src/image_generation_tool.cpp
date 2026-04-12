#include "hermes/tools/image_generation_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <cstdlib>
#include <string>

namespace hermes::tools {

namespace {

hermes::llm::HttpTransport* g_imagegen_transport = nullptr;

std::string handle_image_generate(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    if (!g_imagegen_transport) {
        return tool_error(
            "HTTP transport not available — rebuild with cpr");
    }

    const auto prompt = args.at("prompt").get<std::string>();
    const auto model =
        args.contains("model") ? args["model"].get<std::string>()
                               : std::string("dall-e-3");
    const auto size =
        args.contains("size") ? args["size"].get<std::string>()
                              : std::string("1024x1024");

    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        return tool_error("OPENAI_API_KEY not set");
    }

    nlohmann::json req_body;
    req_body["model"] = model;
    req_body["prompt"] = prompt;
    req_body["size"] = size;
    req_body["n"] = 1;
    req_body["response_format"] = "b64_json";

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = std::string("Bearer ") + api_key;

    auto resp = g_imagegen_transport->post_json(
        "https://api.openai.com/v1/images/generations", headers,
        req_body.dump());

    if (resp.status_code != 200) {
        return tool_error("Image generation API error",
                          {{"status", resp.status_code},
                           {"body", resp.body}});
    }

    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("data") ||
        body["data"].empty()) {
        return tool_error("malformed image generation response");
    }

    const auto& first = body["data"][0];
    nlohmann::json result;
    result["url"] = first.value("url", "");
    result["base64"] = first.value("b64_json", "");
    return tool_result(result);
}

}  // namespace

void register_image_gen_tools(hermes::llm::HttpTransport* transport) {
    g_imagegen_transport = transport;
    auto& reg = ToolRegistry::instance();

    ToolEntry e;
    e.name = "image_generate";
    e.toolset = "image_gen";
    e.description = "Generate an image via DALL-E / Flux / Ideogram";
    e.emoji = "\xF0\x9F\x8E\xA8";  // palette
    e.schema = {
        {"type", "object"},
        {"properties",
         {{"prompt",
           {{"type", "string"},
            {"description", "Image generation prompt"}}},
          {"model",
           {{"type", "string"},
            {"description", "Model name (default dall-e-3)"}}},
          {"size",
           {{"type", "string"},
            {"description", "Image size (default 1024x1024)"}}}}},
        {"required", nlohmann::json::array({"prompt"})}};
    e.handler = handle_image_generate;
    e.check_fn = [] {
        const char* k = std::getenv("OPENAI_API_KEY");
        return k && k[0] != '\0';
    };
    reg.register_tool(std::move(e));
}

}  // namespace hermes::tools
