#include "hermes/tools/registry.hpp"
#include "hermes/tools/vision_tool.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hermes::tools;
using hermes::llm::FakeHttpTransport;

namespace {

class VisionToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        transport_ = std::make_unique<FakeHttpTransport>();
        setenv("OPENAI_API_KEY", "test-key", 1);
        register_vision_tools(transport_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("OPENAI_API_KEY");
    }
    std::unique_ptr<FakeHttpTransport> transport_;
};

TEST_F(VisionToolTest, HappyPath) {
    // First response: image download.
    transport_->enqueue_response({200, "fake-image-bytes", {}});

    // Second response: LLM vision response.
    nlohmann::json llm_resp;
    llm_resp["choices"] = nlohmann::json::array({
        {{"message", {{"content", "A cat sitting on a table"}}}}
    });
    transport_->enqueue_response({200, llm_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "vision_analyze_tool",
        {{"url", "https://example.com/image.png"},
         {"prompt", "Describe this image"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["analysis"], "A cat sitting on a table");
    EXPECT_EQ(transport_->requests().size(), 2u);
}

TEST_F(VisionToolTest, SsrfCheckBlocksPrivateIp) {
    // Should not even attempt HTTP calls.
    auto result = ToolRegistry::instance().dispatch(
        "vision_analyze_tool",
        {{"url", "https://127.0.0.1/evil.png"},
         {"prompt", "describe"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("private"),
              std::string::npos);
    EXPECT_TRUE(transport_->requests().empty());
}

TEST(UrlSafety, PrivateAddresses) {
    EXPECT_TRUE(is_private_url("https://localhost/foo"));
    EXPECT_TRUE(is_private_url("https://127.0.0.1/x"));
    EXPECT_TRUE(is_private_url("https://10.0.0.1/x"));
    EXPECT_TRUE(is_private_url("https://192.168.1.1/x"));
    EXPECT_TRUE(is_private_url("https://172.16.0.1/x"));
    EXPECT_FALSE(is_private_url("https://example.com/x"));
    EXPECT_FALSE(is_private_url("https://8.8.8.8/x"));
}

}  // namespace
