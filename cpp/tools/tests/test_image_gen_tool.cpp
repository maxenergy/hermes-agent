#include "hermes/tools/image_generation_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hermes::tools;
using hermes::llm::FakeHttpTransport;

namespace {

class ImageGenToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        clear_image_model_cache();
        transport_ = std::make_unique<FakeHttpTransport>();
        setenv("OPENAI_API_KEY", "test-key", 1);
        unsetenv("BFL_API_KEY");
        unsetenv("REPLICATE_API_TOKEN");
        unsetenv("IDEOGRAM_API_KEY");
        unsetenv("HERMES_IMAGE_PROVIDER");
        register_image_gen_tools(transport_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        clear_image_model_cache();
        unsetenv("OPENAI_API_KEY");
        unsetenv("BFL_API_KEY");
        unsetenv("REPLICATE_API_TOKEN");
        unsetenv("IDEOGRAM_API_KEY");
        unsetenv("HERMES_IMAGE_PROVIDER");
    }
    std::unique_ptr<FakeHttpTransport> transport_;
};

TEST_F(ImageGenToolTest, HappyPath) {
    nlohmann::json api_resp;
    api_resp["data"] = nlohmann::json::array({
        {{"url", "https://cdn.example.com/img.png"},
         {"b64_json", "base64data"}}
    });
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "image_generate", {{"prompt", "a sunset"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["url"], "https://cdn.example.com/img.png");
    EXPECT_EQ(parsed["base64"], "base64data");
    EXPECT_EQ(parsed["provider"], "openai");
}

TEST_F(ImageGenToolTest, MissingKeyCheckFnFalse) {
    ToolRegistry::instance().clear();
    unsetenv("OPENAI_API_KEY");
    register_image_gen_tools(transport_.get());

    auto defs = ToolRegistry::instance().get_definitions();
    for (const auto& d : defs) {
        EXPECT_NE(d.name, "image_generate");
        EXPECT_NE(d.name, "list_image_models");
    }
}

// ── Flux via BFL ─────────────────────────────────────────────────────
TEST_F(ImageGenToolTest, FluxBflHappyPath) {
    setenv("BFL_API_KEY", "bfl-key", 1);
    nlohmann::json api_resp;
    api_resp["id"] = "task-123";
    api_resp["result"] = {{"sample", "https://bfl.cdn/img.png"}};
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "image_generate",
        {{"prompt", "dragon"},
         {"provider", "flux"},
         {"model", "flux-pro-1.1"},
         {"size", "1024x1024"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["provider"], "flux");
    EXPECT_EQ(parsed["backend"], "bfl");
    EXPECT_EQ(parsed["model"], "flux-pro-1.1");
    EXPECT_EQ(parsed["task_id"], "task-123");
    EXPECT_EQ(parsed["url"], "https://bfl.cdn/img.png");

    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url,
              "https://api.bfl.ml/v1/flux-pro-1.1");
    EXPECT_EQ(transport_->requests()[0].headers.at("x-key"), "bfl-key");
    auto body = nlohmann::json::parse(transport_->requests()[0].body);
    EXPECT_EQ(body["width"], 1024);
    EXPECT_EQ(body["height"], 1024);
}

// ── Flux via Replicate fallback ──────────────────────────────────────
TEST_F(ImageGenToolTest, FluxReplicateFallback) {
    unsetenv("BFL_API_KEY");
    setenv("REPLICATE_API_TOKEN", "rep-key", 1);
    nlohmann::json api_resp;
    api_resp["id"] = "pred-1";
    api_resp["status"] = "starting";
    transport_->enqueue_response({201, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "image_generate",
        {{"prompt", "robot"}, {"provider", "flux"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["provider"], "flux");
    EXPECT_EQ(parsed["backend"], "replicate");
    EXPECT_EQ(parsed["prediction_id"], "pred-1");

    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url,
              "https://api.replicate.com/v1/predictions");
    EXPECT_EQ(transport_->requests()[0].headers.at("Authorization"),
              "Token rep-key");
}

// ── Ideogram v2 ──────────────────────────────────────────────────────
TEST_F(ImageGenToolTest, IdeogramV2HappyPath) {
    setenv("IDEOGRAM_API_KEY", "ideo-key", 1);
    nlohmann::json api_resp;
    api_resp["data"] = nlohmann::json::array({
        {{"url", "https://ideogram.ai/x.png"}, {"is_image_safe", true}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "image_generate",
        {{"prompt", "mountain"},
         {"provider", "ideogram"},
         {"model", "V_2"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["provider"], "ideogram");
    EXPECT_EQ(parsed["api_version"], "v2");
    EXPECT_EQ(parsed["url"], "https://ideogram.ai/x.png");
    EXPECT_EQ(parsed["is_image_safe"], true);

    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url,
              "https://api.ideogram.ai/generate");
    EXPECT_EQ(transport_->requests()[0].headers.at("Api-Key"), "ideo-key");
    auto body = nlohmann::json::parse(transport_->requests()[0].body);
    // v2 uses image_request envelope.
    ASSERT_TRUE(body.contains("image_request"));
    EXPECT_EQ(body["image_request"]["prompt"], "mountain");
    EXPECT_EQ(body["image_request"]["model"], "V_2");
}

// ── Ideogram v3 ──────────────────────────────────────────────────────
TEST_F(ImageGenToolTest, IdeogramV3HappyPath) {
    setenv("IDEOGRAM_API_KEY", "ideo-key", 1);
    nlohmann::json api_resp;
    api_resp["data"] = nlohmann::json::array({
        {{"url", "https://ideogram.ai/y.png"}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "image_generate",
        {{"prompt", "forest"},
         {"provider", "ideogram"},
         {"model", "ideogram-v3"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["api_version"], "v3");
    // v3 uses flat body (no image_request envelope).
    auto body = nlohmann::json::parse(transport_->requests()[0].body);
    EXPECT_EQ(body["prompt"], "forest");
    EXPECT_FALSE(body.contains("image_request"));
}

// ── Unknown provider ────────────────────────────────────────────────
TEST_F(ImageGenToolTest, UnknownProviderError) {
    auto result = ToolRegistry::instance().dispatch(
        "image_generate",
        {{"prompt", "x"}, {"provider", "zzz"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

// ── Model-list cache ────────────────────────────────────────────────
TEST_F(ImageGenToolTest, ListModelsFallbackWhenApiMissing) {
    // No transport response → fallback path used.
    // Use an "offline" provider (flux) whose model-list has no API call.
    auto result = ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "flux"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["provider"], "flux");
    EXPECT_EQ(parsed["source"], "fallback");
    EXPECT_EQ(parsed["cached"], false);
    EXPECT_GT(parsed["models"].size(), 0u);
}

TEST_F(ImageGenToolTest, ListModelsApiSuccessAndCaching) {
    nlohmann::json api_resp;
    api_resp["data"] = nlohmann::json::array({
        {{"id", "dall-e-3"}},
        {{"id", "dall-e-2"}},
        {{"id", "gpt-4"}},  // should be filtered out
    });
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto r1 = ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "openai"}}, {});
    auto p1 = nlohmann::json::parse(r1);
    EXPECT_EQ(p1["source"], "api");
    EXPECT_EQ(p1["cached"], false);
    EXPECT_EQ(p1["models"].size(), 2u);

    // Second call — cache hit, no new request.
    auto r2 = ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "openai"}}, {});
    auto p2 = nlohmann::json::parse(r2);
    EXPECT_EQ(p2["cached"], true);
    EXPECT_EQ(transport_->requests().size(), 1u);
}

TEST_F(ImageGenToolTest, ListModelsCacheTtlExpiry) {
    set_image_model_cache_ttl_seconds(0);  // immediate expiry
    nlohmann::json api_resp;
    api_resp["data"] = nlohmann::json::array({{{"id", "dall-e-3"}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "openai"}}, {});
    ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "openai"}}, {});
    EXPECT_EQ(transport_->requests().size(), 2u);
    set_image_model_cache_ttl_seconds(3600);
}

TEST_F(ImageGenToolTest, ListModelsApiFailureFallsBack) {
    // 500 response triggers fallback path.
    transport_->enqueue_response({500, "oops", {}});
    auto result = ToolRegistry::instance().dispatch(
        "list_image_models", {{"provider", "openai"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["source"], "fallback");
    EXPECT_GT(parsed["models"].size(), 0u);
}

}  // namespace
