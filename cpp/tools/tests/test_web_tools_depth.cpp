// Tests for hermes/tools/web_tools_depth.hpp — pure helpers that mirror
// decisions in tools/web_tools.py.
#include "hermes/tools/web_tools_depth.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace hermes::tools::web;

namespace {

BackendAvailability noAvail() {
    return BackendAvailability{};
}

}  // namespace

TEST(WebDepthBackend, ParseWebBackendNames) {
    EXPECT_EQ(parse_web_backend("firecrawl"), WebBackend::Firecrawl);
    EXPECT_EQ(parse_web_backend("  Firecrawl "), WebBackend::Firecrawl);
    EXPECT_EQ(parse_web_backend("PARALLEL"), WebBackend::Parallel);
    EXPECT_EQ(parse_web_backend("tavily"), WebBackend::Tavily);
    EXPECT_EQ(parse_web_backend("exa"), WebBackend::Exa);
    EXPECT_EQ(parse_web_backend(""), WebBackend::Unknown);
    EXPECT_EQ(parse_web_backend("bing"), WebBackend::Unknown);
}

TEST(WebDepthBackend, WebBackendName) {
    EXPECT_EQ(web_backend_name(WebBackend::Firecrawl), "firecrawl");
    EXPECT_EQ(web_backend_name(WebBackend::Parallel), "parallel");
    EXPECT_EQ(web_backend_name(WebBackend::Tavily), "tavily");
    EXPECT_EQ(web_backend_name(WebBackend::Exa), "exa");
    EXPECT_EQ(web_backend_name(WebBackend::Unknown), "unknown");
}

TEST(WebDepthBackend, ExplicitConfigHonoured) {
    BackendAvailability avail = noAvail();
    avail.exa_key = true;
    EXPECT_EQ(resolve_backend("parallel", avail), WebBackend::Parallel);
    EXPECT_EQ(resolve_backend("tavily", avail), WebBackend::Tavily);
    EXPECT_EQ(resolve_backend("EXA", avail), WebBackend::Exa);
}

TEST(WebDepthBackend, FallbackPriorityFirecrawlFirst) {
    BackendAvailability avail = noAvail();
    avail.firecrawl_key = true;
    avail.parallel_key = true;
    avail.tavily_key = true;
    avail.exa_key = true;
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Firecrawl);
}

TEST(WebDepthBackend, FallbackGatewayCountsAsFirecrawl) {
    BackendAvailability avail = noAvail();
    avail.gateway_ready = true;
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Firecrawl);
}

TEST(WebDepthBackend, FallbackSkipsUnavailable) {
    BackendAvailability avail = noAvail();
    avail.parallel_key = true;
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Parallel);
    avail = noAvail();
    avail.tavily_key = true;
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Tavily);
    avail = noAvail();
    avail.exa_key = true;
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Exa);
}

TEST(WebDepthBackend, FallbackNoneAvailableDefaultsFirecrawl) {
    BackendAvailability avail = noAvail();
    EXPECT_EQ(resolve_backend("", avail), WebBackend::Firecrawl);
}

TEST(WebDepthBackend, IsBackendAvailable) {
    BackendAvailability avail = noAvail();
    EXPECT_FALSE(is_backend_available(WebBackend::Firecrawl, avail));
    avail.firecrawl_url = true;
    EXPECT_TRUE(is_backend_available(WebBackend::Firecrawl, avail));
    avail = noAvail();
    avail.gateway_ready = true;
    EXPECT_TRUE(is_backend_available(WebBackend::Firecrawl, avail));
    EXPECT_FALSE(is_backend_available(WebBackend::Unknown, avail));
}

TEST(WebDepthTavily, NormaliseSearchResults) {
    nlohmann::json body;
    body["results"] = nlohmann::json::array({
        {{"title", "A"}, {"url", "https://a.test"}, {"content", "sa"}},
        {{"title", "B"}, {"url", "https://b.test"}, {"content", "sb"}},
    });
    auto out = normalise_tavily_search_results(body);
    EXPECT_TRUE(out.at("success").get<bool>());
    const auto& web = out.at("data").at("web");
    ASSERT_EQ(web.size(), 2u);
    EXPECT_EQ(web.at(0).at("title").get<std::string>(), "A");
    EXPECT_EQ(web.at(0).at("position").get<int>(), 1);
    EXPECT_EQ(web.at(1).at("description").get<std::string>(), "sb");
    EXPECT_EQ(web.at(1).at("position").get<int>(), 2);
}

TEST(WebDepthTavily, NormaliseSearchMissingResults) {
    nlohmann::json body = nlohmann::json::object();
    auto out = normalise_tavily_search_results(body);
    EXPECT_TRUE(out.at("success").get<bool>());
    EXPECT_TRUE(out.at("data").at("web").empty());
}

TEST(WebDepthTavily, NormaliseDocuments) {
    nlohmann::json body;
    body["results"] = nlohmann::json::array({
        {{"url", "https://x.test"}, {"title", "X"}, {"raw_content", "raw"}},
        {{"url", "https://y.test"}, {"title", "Y"}, {"content", "c"}},
    });
    body["failed_results"] =
        nlohmann::json::array({{{"url", "https://z.test"}, {"error", "403"}}});
    body["failed_urls"] = nlohmann::json::array({"https://q.test"});
    auto docs = normalise_tavily_documents(body, "https://fallback.test");
    ASSERT_EQ(docs.size(), 4u);
    EXPECT_EQ(docs.at(0).at("url").get<std::string>(), "https://x.test");
    EXPECT_EQ(docs.at(0).at("raw_content").get<std::string>(), "raw");
    EXPECT_EQ(docs.at(1).at("raw_content").get<std::string>(), "c");
    EXPECT_EQ(docs.at(2).at("error").get<std::string>(), "403");
    EXPECT_EQ(docs.at(3).at("url").get<std::string>(), "https://q.test");
    EXPECT_EQ(docs.at(3).at("error").get<std::string>(), "extraction failed");
}

TEST(WebDepthTavily, NormaliseDocumentsUsesFallbackUrl) {
    nlohmann::json body;
    body["results"] = nlohmann::json::array({{{"title", "T"}}});
    auto docs =
        normalise_tavily_documents(body, "https://fallback.test/example");
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs.at(0).at("url").get<std::string>(),
              "https://fallback.test/example");
    EXPECT_EQ(docs.at(0).at("metadata").at("sourceURL").get<std::string>(),
              "https://fallback.test/example");
}

TEST(WebDepthExtract, ExtractDataList) {
    nlohmann::json body;
    body["data"] = nlohmann::json::array({{{"url", "u1"}}, "not-an-object",
                                          {{"url", "u2"}}});
    auto out = extract_web_search_results(body);
    ASSERT_EQ(out.size(), 2u);
}

TEST(WebDepthExtract, ExtractDataWeb) {
    nlohmann::json body;
    body["data"]["web"] = nlohmann::json::array({{{"url", "u"}}});
    auto out = extract_web_search_results(body);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out.at(0).at("url").get<std::string>(), "u");
}

TEST(WebDepthExtract, ExtractDataResults) {
    nlohmann::json body;
    body["data"]["results"] = nlohmann::json::array({{{"url", "u"}}});
    auto out = extract_web_search_results(body);
    ASSERT_EQ(out.size(), 1u);
}

TEST(WebDepthExtract, ExtractTopLevelWeb) {
    nlohmann::json body;
    body["web"] = nlohmann::json::array({{{"url", "u"}}, {{"url", "v"}}});
    auto out = extract_web_search_results(body);
    ASSERT_EQ(out.size(), 2u);
}

TEST(WebDepthExtract, ExtractTopLevelResults) {
    nlohmann::json body;
    body["results"] = nlohmann::json::array({{{"url", "u"}}});
    auto out = extract_web_search_results(body);
    ASSERT_EQ(out.size(), 1u);
}

TEST(WebDepthExtract, ExtractEmptyArrays) {
    nlohmann::json body;
    body["data"] = nlohmann::json::array();
    auto out = extract_web_search_results(body);
    EXPECT_TRUE(out.empty());
}

TEST(WebDepthExtract, ScrapePayloadNested) {
    nlohmann::json body;
    body["data"] = {{"markdown", "# hi"}};
    auto out = extract_scrape_payload(body);
    EXPECT_EQ(out.at("markdown").get<std::string>(), "# hi");
}

TEST(WebDepthExtract, ScrapePayloadFlat) {
    nlohmann::json body;
    body["markdown"] = "# hi";
    auto out = extract_scrape_payload(body);
    EXPECT_EQ(out.at("markdown").get<std::string>(), "# hi");
}

TEST(WebDepthContent, CleanBase64Images) {
    std::string input =
        "before (data:image/png;base64,AAAA==) middle "
        "data:image/jpeg;base64,BBBB after";
    auto out = clean_base64_images(input);
    EXPECT_NE(out.find("[BASE64_IMAGE_REMOVED]"), std::string::npos);
    EXPECT_EQ(out.find("base64"), std::string::npos);
}

TEST(WebDepthContent, CleanBase64ImagesNoMatch) {
    std::string input = "plain text";
    EXPECT_EQ(clean_base64_images(input), "plain text");
}

TEST(WebDepthContent, BuildSummariserContextBoth) {
    EXPECT_EQ(build_summariser_context("Title", "http://u"),
              "Title: Title\nSource: http://u\n\n");
}

TEST(WebDepthContent, BuildSummariserContextTitleOnly) {
    EXPECT_EQ(build_summariser_context("T", ""), "Title: T\n\n");
}

TEST(WebDepthContent, BuildSummariserContextUrlOnly) {
    EXPECT_EQ(build_summariser_context("", "http://u"), "Source: http://u\n\n");
}

TEST(WebDepthContent, BuildSummariserContextEmpty) {
    EXPECT_EQ(build_summariser_context("", ""), "");
}

TEST(WebDepthDecide, DecideRefuse) {
    EXPECT_EQ(decide_summariser_mode(kWebSummariserMaxContent + 1, 5000),
              SummariserDecision::Refuse);
}

TEST(WebDepthDecide, DecideSkip) {
    EXPECT_EQ(decide_summariser_mode(10, 5000),
              SummariserDecision::SkipTooShort);
}

TEST(WebDepthDecide, DecideSingleShot) {
    EXPECT_EQ(decide_summariser_mode(10'000, 5000),
              SummariserDecision::SingleShot);
}

TEST(WebDepthDecide, DecideChunked) {
    EXPECT_EQ(decide_summariser_mode(kWebSummariserChunkThreshold + 1, 5000),
              SummariserDecision::Chunked);
}

TEST(WebDepthChunk, SplitContentIntoChunks) {
    const std::string text(250'000, 'x');
    auto chunks = split_content_into_chunks(text, 100'000);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks.at(0).size(), 100'000u);
    EXPECT_EQ(chunks.at(1).size(), 100'000u);
    EXPECT_EQ(chunks.at(2).size(), 50'000u);
}

TEST(WebDepthChunk, SplitExactMultiple) {
    const std::string text(200, 'a');
    auto chunks = split_content_into_chunks(text, 100);
    ASSERT_EQ(chunks.size(), 2u);
}

TEST(WebDepthChunk, FormatChunkInfo) {
    EXPECT_EQ(format_chunk_info(0, 5), "[Processing chunk 1 of 5]");
    EXPECT_EQ(format_chunk_info(4, 5), "[Processing chunk 5 of 5]");
}

TEST(WebDepthChunk, CapSummaryOutputUnder) {
    std::string s = "short";
    EXPECT_EQ(cap_summary_output(s, 100), "short");
}

TEST(WebDepthChunk, CapSummaryOutputOver) {
    std::string s(6000, 'z');
    auto out = cap_summary_output(s, 5000);
    EXPECT_GE(out.size(), 5000u);
    EXPECT_NE(out.find("[... summary truncated"), std::string::npos);
}

TEST(WebDepthChunk, TooLargeMessageFormatsMB) {
    auto msg = format_too_large_message(2'500'000);
    EXPECT_NE(msg.find("2.5MB"), std::string::npos);
}

TEST(WebDepthChunk, TruncationFooterContainsNumbers) {
    auto foot = format_truncation_footer(5000, 20000);
    EXPECT_NE(foot.find("5000"), std::string::npos);
    EXPECT_NE(foot.find("20000"), std::string::npos);
    EXPECT_NE(foot.find("auxiliary.web_extract.timeout"), std::string::npos);
}

TEST(WebDepthEnv, RequiredEnvVarsBasic) {
    auto vars = web_required_env_vars(false);
    EXPECT_EQ(vars.size(), 5u);
    EXPECT_EQ(vars.at(0), "EXA_API_KEY");
    EXPECT_EQ(vars.at(4), "FIRECRAWL_API_URL");
}

TEST(WebDepthEnv, RequiredEnvVarsManaged) {
    auto vars = web_required_env_vars(true);
    EXPECT_EQ(vars.size(), 9u);
    EXPECT_EQ(vars.at(5), "FIRECRAWL_GATEWAY_URL");
    EXPECT_EQ(vars.at(8), "TOOL_GATEWAY_USER_TOKEN");
}

TEST(WebDepthEnv, UrlHostnameLower) {
    EXPECT_EQ(url_hostname_lower("https://Example.COM/foo"), "example.com");
    EXPECT_EQ(url_hostname_lower("http://user:pw@Host:8080/"), "host");
    EXPECT_EQ(url_hostname_lower("https://[::1]:443/"), "::1");
    EXPECT_EQ(url_hostname_lower("bare-host"), "bare-host");
    EXPECT_EQ(url_hostname_lower(""), "");
}

TEST(WebDepthEnv, IsNousAuxiliaryBaseUrl) {
    EXPECT_TRUE(
        is_nous_auxiliary_base_url("https://nousresearch.com/api"));
    EXPECT_TRUE(
        is_nous_auxiliary_base_url("https://api.nousresearch.com/v1"));
    EXPECT_FALSE(is_nous_auxiliary_base_url("https://openrouter.ai/"));
    EXPECT_FALSE(is_nous_auxiliary_base_url(""));
}

TEST(WebDepthParallel, ParseSearchMode) {
    EXPECT_EQ(parse_parallel_search_mode("fast"), "fast");
    EXPECT_EQ(parse_parallel_search_mode(" One-Shot "), "one-shot");
    EXPECT_EQ(parse_parallel_search_mode("agentic"), "agentic");
    EXPECT_EQ(parse_parallel_search_mode("bogus"), "agentic");
    EXPECT_EQ(parse_parallel_search_mode(""), "agentic");
}

TEST(WebDepthNormalise, ResultListFiltersNonObjects) {
    nlohmann::json arr =
        nlohmann::json::array({"s", 1, {{"k", "v"}}, {{"k2", 2}}});
    auto out = normalise_result_list(arr);
    EXPECT_EQ(out.size(), 2u);
}

TEST(WebDepthNormalise, ResultListNonArray) {
    nlohmann::json obj = {{"k", "v"}};
    auto out = normalise_result_list(obj);
    EXPECT_TRUE(out.empty());
}
