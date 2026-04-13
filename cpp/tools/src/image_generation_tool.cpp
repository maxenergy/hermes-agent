// Phase 8+: image generation — DALL-E / Flux / Ideogram backends.
//
// Provider selection via `provider` arg or HERMES_IMAGE_PROVIDER env:
//   - "openai"    → https://api.openai.com/v1/images/generations (DALL-E)
//   - "replicate" → https://api.replicate.com/v1/predictions     (generic)
//   - "flux"      → https://api.bfl.ml/v1/flux-{model}           (Black Forest Labs)
//   - "ideogram"  → https://api.ideogram.ai/generate             (v2 / v3 flag)
//
// Also exposes `list_image_models` which enumerates models supported by
// the selected provider; the result is memoized with a 1-hour TTL.  If
// the provider exposes no model-list endpoint (or the call fails), we
// fall back to a hardcoded short-list of known models.
#include "hermes/tools/image_generation_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hermes::tools {

namespace {

hermes::llm::HttpTransport* g_imagegen_transport = nullptr;

std::string env_or(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return fallback;
    return v;
}

std::string resolve_provider(const nlohmann::json& args) {
    if (args.contains("provider") && args["provider"].is_string()) {
        return args["provider"].get<std::string>();
    }
    auto env = env_or("HERMES_IMAGE_PROVIDER");
    if (!env.empty()) return env;
    return "openai";
}

// ── Model-list cache (1 h TTL) ────────────────────────────────────────
struct ModelListCacheEntry {
    std::chrono::steady_clock::time_point expires_at;
    std::vector<std::string> models;
};

class ModelListCache {
public:
    static ModelListCache& instance() {
        static ModelListCache c;
        return c;
    }
    void set_ttl(std::chrono::seconds ttl) {
        std::lock_guard<std::mutex> lk(mu_);
        ttl_ = ttl;
    }
    bool get(const std::string& provider, std::vector<std::string>* out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(provider);
        if (it == map_.end()) return false;
        if (std::chrono::steady_clock::now() >= it->second.expires_at) {
            map_.erase(it);
            return false;
        }
        *out = it->second.models;
        return true;
    }
    void put(const std::string& provider, std::vector<std::string> m) {
        std::lock_guard<std::mutex> lk(mu_);
        map_[provider] = {std::chrono::steady_clock::now() + ttl_,
                          std::move(m)};
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        map_.clear();
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, ModelListCacheEntry> map_;
    std::chrono::seconds ttl_{3600};
};

// Hardcoded fallback model lists — kept in sync with the Python reference.
std::vector<std::string> fallback_models(const std::string& provider) {
    if (provider == "openai")
        return {"dall-e-3", "dall-e-2", "gpt-image-1"};
    if (provider == "replicate")
        return {"black-forest-labs/flux-schnell",
                "black-forest-labs/flux-dev",
                "stability-ai/stable-diffusion-3.5-large"};
    if (provider == "flux")
        return {"flux-pro-1.1", "flux-pro", "flux-dev", "flux-schnell"};
    if (provider == "ideogram")
        return {"ideogram-v2", "ideogram-v2-turbo",
                "ideogram-v3", "ideogram-v3-turbo"};
    return {};
}

// ── Provider-specific generate implementations ───────────────────────

std::string generate_openai(hermes::llm::HttpTransport* transport,
                            const std::string& prompt,
                            const std::string& model,
                            const std::string& size,
                            const nlohmann::json& /*extra*/) {
    auto key = env_or("OPENAI_API_KEY");
    if (key.empty()) return tool_error("OPENAI_API_KEY not set");
    nlohmann::json req;
    req["model"] = model.empty() ? std::string("dall-e-3") : model;
    req["prompt"] = prompt;
    req["size"] = size.empty() ? std::string("1024x1024") : size;
    req["n"] = 1;
    req["response_format"] = "b64_json";
    std::unordered_map<std::string, std::string> h = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + key},
    };
    auto resp = transport->post_json(
        "https://api.openai.com/v1/images/generations", h, req.dump());
    if (resp.status_code != 200)
        return tool_error("openai image error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded() || !body.contains("data") || body["data"].empty())
        return tool_error("malformed openai response");
    nlohmann::json out;
    out["provider"] = "openai";
    out["model"] = req["model"];
    out["url"] = body["data"][0].value("url", "");
    out["base64"] = body["data"][0].value("b64_json", "");
    return tool_result(out);
}

// Flux via Black Forest Labs API.
std::string generate_flux(hermes::llm::HttpTransport* transport,
                          const std::string& prompt,
                          const std::string& model,
                          const std::string& size,
                          const nlohmann::json& extra) {
    auto key = env_or("BFL_API_KEY");
    if (key.empty()) {
        // Fall back to Replicate if BFL key is absent.
        auto rkey = env_or("REPLICATE_API_TOKEN");
        if (rkey.empty())
            return tool_error(
                "BFL_API_KEY or REPLICATE_API_TOKEN required for Flux");
        nlohmann::json req;
        std::string version = model.empty() ? "black-forest-labs/flux-schnell"
                                            : model;
        req["version"] = version;
        req["input"] = {{"prompt", prompt}};
        if (!size.empty()) req["input"]["size"] = size;
        if (extra.contains("aspect_ratio"))
            req["input"]["aspect_ratio"] = extra["aspect_ratio"];
        std::unordered_map<std::string, std::string> h = {
            {"Content-Type", "application/json"},
            {"Authorization", "Token " + rkey},
        };
        auto resp = transport->post_json(
            "https://api.replicate.com/v1/predictions", h, req.dump());
        if (resp.status_code != 200 && resp.status_code != 201)
            return tool_error("replicate flux error",
                              {{"status", resp.status_code},
                               {"body", resp.body}});
        auto body = nlohmann::json::parse(resp.body, nullptr, false);
        if (body.is_discarded())
            return tool_error("malformed replicate response");
        nlohmann::json out;
        out["provider"] = "flux";
        out["backend"] = "replicate";
        out["model"] = version;
        out["prediction_id"] = body.value("id", "");
        out["status"] = body.value("status", "");
        if (body.contains("output")) out["url"] = body["output"];
        return tool_result(out);
    }

    // Direct BFL API.
    std::string endpoint_model = model.empty() ? "flux-pro-1.1" : model;
    std::string url = "https://api.bfl.ml/v1/" + endpoint_model;
    nlohmann::json req;
    req["prompt"] = prompt;
    if (!size.empty()) {
        // BFL uses width/height, not a "1024x1024" string — best-effort parse.
        auto x = size.find('x');
        if (x != std::string::npos) {
            req["width"] = std::stoi(size.substr(0, x));
            req["height"] = std::stoi(size.substr(x + 1));
        }
    }
    if (extra.contains("seed")) req["seed"] = extra["seed"];
    if (extra.contains("steps")) req["steps"] = extra["steps"];
    std::unordered_map<std::string, std::string> h = {
        {"Content-Type", "application/json"},
        {"x-key", key},
    };
    auto resp = transport->post_json(url, h, req.dump());
    if (resp.status_code != 200 && resp.status_code != 202)
        return tool_error("flux api error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded())
        return tool_error("malformed flux response");
    nlohmann::json out;
    out["provider"] = "flux";
    out["backend"] = "bfl";
    out["model"] = endpoint_model;
    out["task_id"] = body.value("id", "");
    if (body.contains("result")) {
        const auto& r = body["result"];
        if (r.is_object()) out["url"] = r.value("sample", "");
    }
    if (body.contains("sample")) out["url"] = body["sample"];
    return tool_result(out);
}

// Ideogram v2 / v3.
std::string generate_ideogram(hermes::llm::HttpTransport* transport,
                              const std::string& prompt,
                              const std::string& model,
                              const std::string& size,
                              const nlohmann::json& extra) {
    auto key = env_or("IDEOGRAM_API_KEY");
    if (key.empty()) return tool_error("IDEOGRAM_API_KEY not set");

    // Determine API version — default v2 unless model mentions "v3".
    std::string version = extra.value(
        "api_version", std::string(
            (model.find("v3") != std::string::npos) ? "v3" : "v2"));

    nlohmann::json req;
    if (version == "v3") {
        req["prompt"] = prompt;
        if (!model.empty()) req["model"] = model;
        if (!size.empty()) req["resolution"] = size;
        if (extra.contains("aspect_ratio"))
            req["aspect_ratio"] = extra["aspect_ratio"];
        if (extra.contains("style_type"))
            req["style_type"] = extra["style_type"];
    } else {
        nlohmann::json ir;
        ir["prompt"] = prompt;
        ir["model"] = model.empty() ? std::string("V_2") : model;
        if (extra.contains("aspect_ratio"))
            ir["aspect_ratio"] = extra["aspect_ratio"];
        if (extra.contains("style_type"))
            ir["style_type"] = extra["style_type"];
        req["image_request"] = ir;
    }
    std::unordered_map<std::string, std::string> h = {
        {"Content-Type", "application/json"},
        {"Api-Key", key},
    };
    auto resp = transport->post_json(
        "https://api.ideogram.ai/generate", h, req.dump());
    if (resp.status_code != 200)
        return tool_error("ideogram api error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded())
        return tool_error("malformed ideogram response");
    nlohmann::json out;
    out["provider"] = "ideogram";
    out["api_version"] = version;
    out["model"] = model;
    if (body.contains("data") && body["data"].is_array() &&
        !body["data"].empty()) {
        out["url"] = body["data"][0].value("url", "");
        if (body["data"][0].contains("is_image_safe"))
            out["is_image_safe"] = body["data"][0]["is_image_safe"];
    }
    return tool_result(out);
}

std::string generate_replicate(hermes::llm::HttpTransport* transport,
                               const std::string& prompt,
                               const std::string& model,
                               const std::string& /*size*/,
                               const nlohmann::json& extra) {
    auto key = env_or("REPLICATE_API_TOKEN");
    if (key.empty()) return tool_error("REPLICATE_API_TOKEN not set");
    nlohmann::json req;
    req["version"] = model.empty() ? "black-forest-labs/flux-schnell" : model;
    req["input"] = {{"prompt", prompt}};
    if (extra.contains("aspect_ratio"))
        req["input"]["aspect_ratio"] = extra["aspect_ratio"];
    std::unordered_map<std::string, std::string> h = {
        {"Content-Type", "application/json"},
        {"Authorization", "Token " + key},
    };
    auto resp = transport->post_json(
        "https://api.replicate.com/v1/predictions", h, req.dump());
    if (resp.status_code != 200 && resp.status_code != 201)
        return tool_error("replicate error",
                          {{"status", resp.status_code}, {"body", resp.body}});
    auto body = nlohmann::json::parse(resp.body, nullptr, false);
    if (body.is_discarded())
        return tool_error("malformed replicate response");
    nlohmann::json out;
    out["provider"] = "replicate";
    out["model"] = req["version"];
    out["prediction_id"] = body.value("id", "");
    out["status"] = body.value("status", "");
    if (body.contains("output")) out["url"] = body["output"];
    return tool_result(out);
}

// ── Handlers ─────────────────────────────────────────────────────────

std::string handle_image_generate(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* transport = g_imagegen_transport ? g_imagegen_transport
                                          : hermes::llm::get_default_transport();
    assert(transport && "HTTP transport should always be available");

    const auto prompt = args.at("prompt").get<std::string>();
    const auto model =
        args.contains("model") ? args["model"].get<std::string>()
                               : std::string();
    const auto size =
        args.contains("size") ? args["size"].get<std::string>()
                              : std::string("1024x1024");
    const std::string provider = resolve_provider(args);

    nlohmann::json extra = args;
    extra.erase("prompt");
    extra.erase("model");
    extra.erase("size");
    extra.erase("provider");

    if (provider == "openai")
        return generate_openai(transport, prompt, model, size, extra);
    if (provider == "flux" || provider == "bfl")
        return generate_flux(transport, prompt, model, size, extra);
    if (provider == "ideogram")
        return generate_ideogram(transport, prompt, model, size, extra);
    if (provider == "replicate")
        return generate_replicate(transport, prompt, model, size, extra);

    return tool_error("unknown image provider", {{"provider", provider}});
}

std::string handle_list_image_models(const nlohmann::json& args,
                                     const ToolContext& /*ctx*/) {
    auto* transport = g_imagegen_transport ? g_imagegen_transport
                                          : hermes::llm::get_default_transport();
    assert(transport);
    const std::string provider = resolve_provider(args);

    std::vector<std::string> models;
    if (ModelListCache::instance().get(provider, &models)) {
        nlohmann::json out;
        out["provider"] = provider;
        out["models"] = models;
        out["cached"] = true;
        return tool_result(out);
    }

    bool fetched = false;
    // Only OpenAI and Replicate expose a live model-list endpoint.
    if (provider == "openai") {
        auto key = env_or("OPENAI_API_KEY");
        if (!key.empty()) {
            std::unordered_map<std::string, std::string> h = {
                {"Authorization", "Bearer " + key},
            };
            auto resp = transport->get("https://api.openai.com/v1/models", h);
            if (resp.status_code == 200) {
                auto body = nlohmann::json::parse(resp.body, nullptr, false);
                if (!body.is_discarded() && body.contains("data")) {
                    for (const auto& m : body["data"]) {
                        auto id = m.value("id", std::string());
                        if (id.find("dall-e") != std::string::npos ||
                            id.find("image") != std::string::npos) {
                            models.push_back(id);
                        }
                    }
                    fetched = !models.empty();
                }
            }
        }
    } else if (provider == "replicate") {
        auto key = env_or("REPLICATE_API_TOKEN");
        if (!key.empty()) {
            std::unordered_map<std::string, std::string> h = {
                {"Authorization", "Token " + key},
            };
            auto resp = transport->get(
                "https://api.replicate.com/v1/collections/text-to-image", h);
            if (resp.status_code == 200) {
                auto body = nlohmann::json::parse(resp.body, nullptr, false);
                if (!body.is_discarded() && body.contains("models")) {
                    for (const auto& m : body["models"]) {
                        auto owner = m.value("owner", std::string());
                        auto name = m.value("name", std::string());
                        if (!owner.empty() && !name.empty())
                            models.push_back(owner + "/" + name);
                    }
                    fetched = !models.empty();
                }
            }
        }
    }

    if (!fetched) models = fallback_models(provider);
    ModelListCache::instance().put(provider, models);

    nlohmann::json out;
    out["provider"] = provider;
    out["models"] = models;
    out["cached"] = false;
    out["source"] = fetched ? std::string("api") : std::string("fallback");
    return tool_result(out);
}

bool any_image_key() {
    static const char* keys[] = {"OPENAI_API_KEY", "BFL_API_KEY",
                                 "REPLICATE_API_TOKEN", "IDEOGRAM_API_KEY"};
    for (const char* k : keys) {
        const char* v = std::getenv(k);
        if (v && v[0] != '\0') return true;
    }
    return false;
}

}  // namespace

// Test hooks.
void clear_image_model_cache() { ModelListCache::instance().clear(); }
void set_image_model_cache_ttl_seconds(int ttl) {
    ModelListCache::instance().set_ttl(std::chrono::seconds(ttl));
}

void register_image_gen_tools(hermes::llm::HttpTransport* transport) {
    g_imagegen_transport = transport;
    auto& reg = ToolRegistry::instance();

    {
        ToolEntry e;
        e.name = "image_generate";
        e.toolset = "image_gen";
        e.description =
            "Generate an image via pluggable provider "
            "(openai|flux|ideogram|replicate).";
        e.emoji = "\xF0\x9F\x8E\xA8";
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"prompt",
               {{"type", "string"},
                {"description", "Image generation prompt"}}},
              {"provider",
               {{"type", "string"},
                {"description",
                 "openai|flux|ideogram|replicate "
                 "(default: HERMES_IMAGE_PROVIDER or openai)"}}},
              {"model",
               {{"type", "string"},
                {"description",
                 "Model name — e.g. dall-e-3, flux-pro-1.1, ideogram-v3"}}},
              {"size",
               {{"type", "string"},
                {"description", "Image size (default 1024x1024)"}}}}},
            {"required", nlohmann::json::array({"prompt"})}};
        e.handler = handle_image_generate;
        e.check_fn = [] { return any_image_key(); };
        reg.register_tool(std::move(e));
    }

    {
        ToolEntry e;
        e.name = "list_image_models";
        e.toolset = "image_gen";
        e.description =
            "List image-generation models supported by the selected provider. "
            "Memoized for 1 h; falls back to a hardcoded list if the API is "
            "unreachable.";
        e.emoji = "\xF0\x9F\x93\x9A";
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"provider",
               {{"type", "string"},
                {"description",
                 "openai|flux|ideogram|replicate "
                 "(default: HERMES_IMAGE_PROVIDER or openai)"}}}}},
            {"required", nlohmann::json::array()}};
        e.handler = handle_list_image_models;
        e.check_fn = [] { return any_image_key(); };
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
