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
        clear_web_search_cache();
        transport_ = std::make_unique<FakeHttpTransport>();
        setenv("EXA_API_KEY", "test-key", 1);
        setenv("FIRECRAWL_API_KEY", "test-key", 1);
        unsetenv("TAVILY_API_KEY");
        unsetenv("PARALLEL_API_KEY");
        unsetenv("BRAVE_API_KEY");
        unsetenv("GOOGLE_API_KEY");
        unsetenv("GOOGLE_CSE_ID");
        unsetenv("HERMES_WEB_PROVIDER");
        register_web_tools(transport_.get());
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        clear_web_search_cache();
        unsetenv("EXA_API_KEY");
        unsetenv("FIRECRAWL_API_KEY");
        unsetenv("TAVILY_API_KEY");
        unsetenv("PARALLEL_API_KEY");
        unsetenv("BRAVE_API_KEY");
        unsetenv("GOOGLE_API_KEY");
        unsetenv("GOOGLE_CSE_ID");
        unsetenv("HERMES_WEB_PROVIDER");
    }
    std::unique_ptr<FakeHttpTransport> transport_;
};

TEST_F(WebToolsTest, SearchHappyPathExa) {
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
    EXPECT_EQ(parsed["provider"], "exa");

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

    auto defs = ToolRegistry::instance().get_definitions();
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

// ── Tavily backend ───────────────────────────────────────────────────
TEST_F(WebToolsTest, TavilyHappyPath) {
    setenv("TAVILY_API_KEY", "tav-key", 1);
    nlohmann::json api_resp;
    api_resp["answer"] = "short summary";
    api_resp["results"] = nlohmann::json::array({
        {{"title", "T1"}, {"url", "https://t1.com"}, {"content", "body1"}},
        {{"title", "T2"}, {"url", "https://t2.com"}, {"content", "body2"}}
    });
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_search",
        {{"query", "quantum"}, {"provider", "tavily"}, {"num_results", 2}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("results"));
    EXPECT_EQ(parsed["results"].size(), 2u);
    EXPECT_EQ(parsed["results"][0]["snippet"], "body1");
    EXPECT_EQ(parsed["provider"], "tavily");
    EXPECT_EQ(parsed["answer"], "short summary");

    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url, "https://api.tavily.com/search");
    // Body should include api_key inline.
    auto body = nlohmann::json::parse(transport_->requests()[0].body);
    EXPECT_EQ(body["api_key"], "tav-key");
    EXPECT_EQ(body["max_results"], 2);
}

TEST_F(WebToolsTest, TavilyMissingKeyReturnsError) {
    unsetenv("TAVILY_API_KEY");
    auto result = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "x"}, {"provider", "tavily"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

// ── Parallel.ai backend ──────────────────────────────────────────────
TEST_F(WebToolsTest, ParallelHappyPath) {
    setenv("PARALLEL_API_KEY", "par-key", 1);
    nlohmann::json api_resp;
    api_resp["results"] = nlohmann::json::array({
        {{"title", "Paper"},
         {"url", "https://arxiv.org/x"},
         {"excerpts", nlohmann::json::array({"first excerpt",
                                              "second excerpt"})}},
    });
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_search",
        {{"query", "LLM alignment"}, {"provider", "parallel"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_EQ(parsed["results"].size(), 1u);
    EXPECT_EQ(parsed["results"][0]["title"], "Paper");
    auto snippet = parsed["results"][0]["snippet"].get<std::string>();
    EXPECT_NE(snippet.find("first excerpt"), std::string::npos);
    EXPECT_NE(snippet.find("second excerpt"), std::string::npos);

    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url,
              "https://api.parallel.ai/v1/search");
    EXPECT_EQ(transport_->requests()[0].headers.at("x-api-key"), "par-key");
}

// ── Unknown provider ────────────────────────────────────────────────
TEST_F(WebToolsTest, UnknownProviderReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "x"}, {"provider", "nope"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

// ── HERMES_WEB_PROVIDER env var selection ────────────────────────────
TEST_F(WebToolsTest, ProviderFromEnvVar) {
    setenv("TAVILY_API_KEY", "tk", 1);
    setenv("HERMES_WEB_PROVIDER", "tavily", 1);
    nlohmann::json api_resp;
    api_resp["results"] = nlohmann::json::array({
        {{"title", "E"}, {"url", "https://e.com"}, {"content", "c"}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto result = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "z"}}, {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["provider"], "tavily");
    ASSERT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(transport_->requests()[0].url, "https://api.tavily.com/search");
}

// ── Cache ───────────────────────────────────────────────────────────
TEST_F(WebToolsTest, CacheHitShortCircuitsNetwork) {
    nlohmann::json api_resp;
    api_resp["results"] = nlohmann::json::array({
        {{"title", "Once"}, {"url", "https://once.com"}, {"text", "s"}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    auto r1 = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "same"}}, {});
    auto p1 = nlohmann::json::parse(r1);
    EXPECT_EQ(p1["cached"], false);

    // Second call with identical query+opts should hit the cache and NOT
    // make a second network request (transport queue is empty anyway, so
    // a miss would fail the test here by running into an empty queue).
    auto r2 = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "same"}}, {});
    auto p2 = nlohmann::json::parse(r2);
    EXPECT_EQ(p2["cached"], true);
    EXPECT_EQ(p2["results"].size(), 1u);
    EXPECT_EQ(transport_->requests().size(), 1u);
    EXPECT_EQ(web_search_cache_size(), 1u);
}

TEST_F(WebToolsTest, CacheKeyedByProvider) {
    setenv("TAVILY_API_KEY", "tk", 1);
    // Two different providers for the same query → two cache entries.
    nlohmann::json exa_resp;
    exa_resp["results"] = nlohmann::json::array({
        {{"title", "E"}, {"url", "https://e.com"}, {"text", "s"}}});
    nlohmann::json tav_resp;
    tav_resp["results"] = nlohmann::json::array({
        {{"title", "T"}, {"url", "https://t.com"}, {"content", "s"}}});
    transport_->enqueue_response({200, exa_resp.dump(), {}});
    transport_->enqueue_response({200, tav_resp.dump(), {}});

    ToolRegistry::instance().dispatch("web_search", {{"query", "q"}}, {});
    ToolRegistry::instance().dispatch(
        "web_search", {{"query", "q"}, {"provider", "tavily"}}, {});
    EXPECT_EQ(web_search_cache_size(), 2u);
    EXPECT_EQ(transport_->requests().size(), 2u);
}

TEST_F(WebToolsTest, CacheExpiresOnTtl) {
    set_web_search_cache_ttl_seconds(0);  // Immediate expiry.
    nlohmann::json api_resp;
    api_resp["results"] = nlohmann::json::array({
        {{"title", "A"}, {"url", "https://a.com"}, {"text", "s"}}});
    transport_->enqueue_response({200, api_resp.dump(), {}});
    transport_->enqueue_response({200, api_resp.dump(), {}});

    ToolRegistry::instance().dispatch("web_search", {{"query", "x"}}, {});
    auto r2 = ToolRegistry::instance().dispatch(
        "web_search", {{"query", "x"}}, {});
    // With TTL 0, every call misses and re-fetches.
    EXPECT_EQ(transport_->requests().size(), 2u);
    auto p2 = nlohmann::json::parse(r2);
    EXPECT_EQ(p2["cached"], false);
    // Reset ttl for other tests in suite.
    set_web_search_cache_ttl_seconds(60);
}

// ── Helper-function coverage (web:: namespace) ───────────────────────

TEST(WebUtil, SupportedProvidersContainsExpected) {
    const auto& p = web::supported_providers();
    EXPECT_NE(std::find(p.begin(), p.end(), "exa"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "tavily"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "brave"), p.end());
    EXPECT_NE(std::find(p.begin(), p.end(), "google"), p.end());
}

TEST(WebUtil, IsSupportedProvider) {
    EXPECT_TRUE(web::is_supported_provider("exa"));
    EXPECT_TRUE(web::is_supported_provider("tavily"));
    EXPECT_FALSE(web::is_supported_provider("bogus"));
    EXPECT_FALSE(web::is_supported_provider(""));
}

TEST(WebUtil, UrlEncodePlain) {
    EXPECT_EQ(web::url_encode("hello"), "hello");
    EXPECT_EQ(web::url_encode("abc123"), "abc123");
    EXPECT_EQ(web::url_encode("a-b.c_d~e"), "a-b.c_d~e");
}

TEST(WebUtil, UrlEncodeSpaces) {
    EXPECT_EQ(web::url_encode("hello world"), "hello+world");
}

TEST(WebUtil, UrlEncodeReserved) {
    EXPECT_EQ(web::url_encode("a&b=c"), "a%26b%3dc");
    EXPECT_EQ(web::url_encode("100%"), "100%25");
    EXPECT_EQ(web::url_encode("?x=1"), "%3fx%3d1");
}

TEST(WebUtil, CacheKeyStable) {
    auto k1 = web::cache_key("exa", "foo",
                             nlohmann::json{{"num_results", 5}});
    auto k2 = web::cache_key("exa", "foo",
                             nlohmann::json{{"num_results", 5}});
    EXPECT_EQ(k1, k2);
    EXPECT_TRUE(k1.find("exa:") == 0);
}

TEST(WebUtil, CacheKeyDiffersByProvider) {
    auto k1 = web::cache_key("exa", "foo", {});
    auto k2 = web::cache_key("tavily", "foo", {});
    EXPECT_NE(k1, k2);
}

TEST(WebUtil, NormalizeTavilySearchEmpty) {
    auto r = web::normalize_tavily_search(nlohmann::json::object());
    EXPECT_TRUE(r["results"].is_array());
    EXPECT_TRUE(r["results"].empty());
}

TEST(WebUtil, NormalizeTavilySearchWithResults) {
    nlohmann::json body = {
        {"answer", "it is 42"},
        {"results",
         nlohmann::json::array(
             {{{"title", "Hitchhiker's"},
               {"url", "https://x/1"},
               {"content", "42 is the answer"}}})}};
    auto r = web::normalize_tavily_search(body);
    EXPECT_EQ(r["answer"].get<std::string>(), "it is 42");
    ASSERT_EQ(r["results"].size(), 1u);
    EXPECT_EQ(r["results"][0]["title"].get<std::string>(), "Hitchhiker's");
    EXPECT_EQ(r["results"][0]["snippet"].get<std::string>(),
              "42 is the answer");
    EXPECT_EQ(r["results"][0]["position"].get<int>(), 1);
}

TEST(WebUtil, NormalizeTavilyDocumentsBasic) {
    nlohmann::json body = {
        {"results",
         nlohmann::json::array(
             {{{"url", "https://x"},
               {"title", "Page"},
               {"raw_content", "full text"}}})}};
    auto docs = web::normalize_tavily_documents(body);
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs[0]["url"].get<std::string>(), "https://x");
    EXPECT_EQ(docs[0]["raw_content"].get<std::string>(), "full text");
    EXPECT_EQ(docs[0]["metadata"]["sourceURL"].get<std::string>(), "https://x");
}

TEST(WebUtil, NormalizeTavilyDocumentsFailedResults) {
    nlohmann::json body = {
        {"failed_results",
         nlohmann::json::array(
             {{{"url", "https://bad"}, {"error", "timeout"}}})},
        {"failed_urls", nlohmann::json::array({"https://other"})}};
    auto docs = web::normalize_tavily_documents(body, "https://fallback");
    ASSERT_EQ(docs.size(), 2u);
    EXPECT_EQ(docs[0]["url"].get<std::string>(), "https://bad");
    EXPECT_EQ(docs[0]["error"].get<std::string>(), "timeout");
    EXPECT_EQ(docs[1]["url"].get<std::string>(), "https://other");
    EXPECT_EQ(docs[1]["error"].get<std::string>(), "extraction failed");
}

TEST(WebUtil, NormalizeTavilyDocumentsFallbackUrl) {
    nlohmann::json body = {
        {"results",
         nlohmann::json::array({{{"title", "T"}, {"content", "C"}}})}};
    auto docs = web::normalize_tavily_documents(body, "https://fb");
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs[0]["url"].get<std::string>(), "https://fb");
}

}  // namespace
