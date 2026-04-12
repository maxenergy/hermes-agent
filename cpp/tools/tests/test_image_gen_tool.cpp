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
        transport_ = std::make_unique<FakeHttpTransport>();
        setenv("OPENAI_API_KEY", "test-key", 1);
        register_image_gen_tools(transport_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("OPENAI_API_KEY");
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
}

TEST_F(ImageGenToolTest, MissingKeyCheckFnFalse) {
    ToolRegistry::instance().clear();
    unsetenv("OPENAI_API_KEY");
    register_image_gen_tools(transport_.get());

    auto defs = ToolRegistry::instance().get_definitions();
    for (const auto& d : defs) {
        EXPECT_NE(d.name, "image_generate");
    }
}

}  // namespace
