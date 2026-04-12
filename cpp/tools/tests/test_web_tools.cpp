#include "hermes/tools/registry.hpp"
#include "hermes/tools/web_tools.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace hermes::tools;
using hermes::llm::FakeHttpTransport;

namespace {

class WebToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        transport_ = std::make_unique<FakeHttpTransport>();
        // Set env vars so check_fn passes.
        setenv("EXA_API_KEY", "test-key", 1);
        setenv("FIRECRAWL_API_KEY", "test-key", 1);
        register_web_tools(transport_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        unsetenv("EXA_API_KEY");
        unsetenv("FIRECRAWL_API_KEY");
    }
    std::unique_ptr<FakeHttpTransport> transport_;
};

TEST_F(WebToolsTest, SearchHappyPath) {
    nlohmann::json api_resp;
    api_resp["results"] = nlohmann::json::array({
        {{"title", "Example"}, {"url", "https://example.com"}, {"text", "snippet"}}
    });
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "test"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("results"));
    EXPECT_EQ(parsed["results"].size(), 1u);
    EXPECT_EQ(parsed["results"][0]["title"], "Example");
    EXPECT_EQ(parsed["results"][0]["url"], "https://example.com");

    // Verify the request was sent to Exa.
    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url, "https://api.exa.ai/search");
}

TEST_F(WebToolsTest, ExtractHappyPath) {
    nlohmann::json api_resp;
    api_resp["data"] = {
        {"markdown", "Hello world"},
        {"title", "Page Title"}
    };
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_extract", {{"url", "https://example.com"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["content"], "Hello world");
    EXPECT_EQ(parsed["title"], "Page Title");
    EXPECT_EQ(parsed["url"], "https://example.com");
}

TEST_F(WebToolsTest, MissingApiKeyCheckFnFalse) {
    ToolRegistry::instance().clear();
    unsetenv("EXA_API_KEY");
    unsetenv("FIRECRAWL_API_KEY");
    register_web_tools(transport_.get());

    // get_definitions with empty enabled = all tools; check_fn should fail.
    auto defs = ToolRegistry::instance().get_definitions();
    // Neither tool should appear because check_fn returns false.
    for (const auto& d : defs) {
        EXPECT_NE(d.name, "web_search");
        EXPECT_NE(d.name, "web_extract");
    }
}

TEST_F(WebToolsTest, MalformedResponseReturnsError) {
    transport_->enqueue_response({200, "not json at all", {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "test"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

}  // namespace
