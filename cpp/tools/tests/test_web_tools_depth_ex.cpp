// Tests for hermes/tools/web_tools_depth_ex.hpp.
#include "hermes/tools/web_tools_depth_ex.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace hermes::tools::web::depth_ex;
using json = nlohmann::json;

TEST(WebDepthExQuery, RejectEmpty) {
    EXPECT_NE(validate_query(""), "");
    EXPECT_NE(validate_query("   "), "");
}

TEST(WebDepthExQuery, RejectOversized) {
    std::string big(kMaxQueryChars + 1, 'x');
    EXPECT_NE(validate_query(big), "");
}

TEST(WebDepthExQuery, AcceptReasonable) {
    EXPECT_EQ(validate_query("hello world"), "");
    EXPECT_EQ(validate_query(std::string(kMaxQueryChars, 'x')), "");
}

TEST(WebDepthExUrl, IsHttpUrl) {
    EXPECT_TRUE(is_http_url("http://example.com"));
    EXPECT_TRUE(is_http_url("https://example.com/path"));
    EXPECT_FALSE(is_http_url("ftp://example.com"));
    EXPECT_FALSE(is_http_url("http://"));
    EXPECT_FALSE(is_http_url(""));
    EXPECT_FALSE(is_http_url("example.com"));
}

TEST(WebDepthExBatch, EmptyRejected) {
    EXPECT_NE(validate_url_batch({}), "");
}

TEST(WebDepthExBatch, OversizedRejected) {
    std::vector<std::string> big(kMaxBatchUrls + 1u, "https://x");
    EXPECT_NE(validate_url_batch(big), "");
}

TEST(WebDepthExBatch, BadUrlRejected) {
    std::vector<std::string> in{"https://ok", "notaurl"};
    auto err = validate_url_batch(in);
    EXPECT_NE(err.find("notaurl"), std::string::npos);
}

TEST(WebDepthExBatch, AllValid) {
    std::vector<std::string> in{"https://a", "http://b/c"};
    EXPECT_EQ(validate_url_batch(in), "");
}

TEST(WebDepthExUnwrap, UnwrapsDataObject) {
    json in = json::object();
    in["data"] = json::object();
    in["data"]["foo"] = 1;
    auto out = unwrap_data(in);
    EXPECT_TRUE(out.is_object());
    EXPECT_EQ(out["foo"].get<int>(), 1);
}

TEST(WebDepthExUnwrap, UnwrapsDataArray) {
    json in = json::object();
    in["data"] = json::array({"a", "b"});
    auto out = unwrap_data(in);
    EXPECT_TRUE(out.is_array());
    EXPECT_EQ(out.size(), 2u);
}

TEST(WebDepthExUnwrap, NonObjectPasses) {
    json in = json::array({1, 2});
    EXPECT_EQ(unwrap_data(in), in);
}

TEST(WebDepthExUnwrap, MissingDataPasses) {
    json in = json::object();
    in["other"] = 1;
    EXPECT_EQ(unwrap_data(in), in);
}

TEST(WebDepthExPluck, FirstKeyMatches) {
    json in = json::object();
    in["results"] = json::array({json::object({{"url", "a"}})});
    auto out = pluck_first_array(in, {"results", "web"});
    ASSERT_EQ(out.size(), 1u);
}

TEST(WebDepthExPluck, NestedDotPath) {
    json in = json::object();
    in["data"] = json::object();
    in["data"]["web"] = json::array({"x", "y"});
    auto out = pluck_first_array(in, {"data.web"});
    ASSERT_EQ(out.size(), 2u);
}

TEST(WebDepthExPluck, EmptyArraySkipped) {
    json in = json::object();
    in["results"] = json::array();
    in["web"] = json::array({1});
    auto out = pluck_first_array(in, {"results", "web"});
    ASSERT_EQ(out.size(), 1u);
}

TEST(WebDepthExPluck, NoMatchReturnsEmpty) {
    json in = json::object();
    in["other"] = json::array({1});
    auto out = pluck_first_array(in, {"data", "web"});
    EXPECT_TRUE(out.is_array());
    EXPECT_EQ(out.size(), 0u);
}

TEST(WebDepthExPlain, KeepsKeysDropsUnderscore) {
    json in = json::object();
    in["foo"] = 1;
    in["_private"] = 2;
    auto out = to_plain_object(in);
    EXPECT_EQ(out["foo"].get<int>(), 1);
    EXPECT_FALSE(out.contains("_private"));
}

TEST(WebDepthExPlain, NonObjectReturnsNull) {
    EXPECT_TRUE(to_plain_object(json::array()).is_null());
}

TEST(WebDepthExDoc, UsableWithUrlAndContent) {
    json doc = json::object();
    doc["url"] = "https://x";
    doc["content"] = "body";
    EXPECT_TRUE(document_is_usable(doc));
}

TEST(WebDepthExDoc, RejectsMissingUrl) {
    json doc = json::object();
    doc["content"] = "body";
    EXPECT_FALSE(document_is_usable(doc));
}

TEST(WebDepthExDoc, RejectsEmptyContent) {
    json doc = json::object();
    doc["url"] = "https://x";
    doc["content"] = "   ";
    EXPECT_FALSE(document_is_usable(doc));
}

TEST(WebDepthExDoc, StripsNoise) {
    json doc = json::object();
    doc["url"] = "u";
    doc["content"] = "c";
    doc["raw_content"] = "big raw";
    doc["html"] = "<html>";
    doc["screenshot"] = "data:image";
    doc["links"] = json::array({"a", "b"});
    auto out = strip_noise_fields(doc);
    EXPECT_FALSE(out.contains("raw_content"));
    EXPECT_FALSE(out.contains("html"));
    EXPECT_FALSE(out.contains("screenshot"));
    EXPECT_FALSE(out.contains("links"));
    EXPECT_EQ(out["url"].get<std::string>(), "u");
}

TEST(WebDepthExBody, ParallelCapsResults) {
    auto b = build_parallel_search_body("hi", "fast", 500);
    EXPECT_EQ(b["query"].get<std::string>(), "hi");
    EXPECT_EQ(b["search_mode"].get<std::string>(), "fast");
    EXPECT_EQ(b["max_results"].get<std::size_t>(), 20u);
}

TEST(WebDepthExBody, ParallelPassesThroughSmaller) {
    auto b = build_parallel_search_body("hi", "agentic", 5);
    EXPECT_EQ(b["max_results"].get<std::size_t>(), 5u);
}

TEST(WebDepthExBody, Exa) {
    auto b = build_exa_search_body("hi", 10, true);
    EXPECT_EQ(b["query"].get<std::string>(), "hi");
    EXPECT_EQ(b["numResults"].get<std::size_t>(), 10u);
    EXPECT_TRUE(b["useAutoprompt"].get<bool>());
}

TEST(WebDepthExError, NoErrorWhenClean) {
    json r = json::object();
    r["success"] = true;
    r["data"] = json::array();
    EXPECT_EQ(firecrawl_error_message(r), "");
}

TEST(WebDepthExError, TopLevelErrorString) {
    json r = json::object();
    r["error"] = "rate limited";
    EXPECT_EQ(firecrawl_error_message(r), "rate limited");
}

TEST(WebDepthExError, SuccessFalseWithMessage) {
    json r = json::object();
    r["success"] = false;
    r["message"] = "quota exceeded";
    EXPECT_EQ(firecrawl_error_message(r), "quota exceeded");
}

TEST(WebDepthExError, SuccessFalseFallback) {
    json r = json::object();
    r["success"] = false;
    EXPECT_NE(firecrawl_error_message(r), "");
}

TEST(WebDepthExError, NonObjectEmpty) {
    EXPECT_EQ(firecrawl_error_message(json::array({1})), "");
}

TEST(WebDepthExRetryAfter, Numeric) {
    EXPECT_EQ(parse_retry_after_seconds("30"), 30);
    EXPECT_EQ(parse_retry_after_seconds(" 120 "), 120);
    EXPECT_EQ(parse_retry_after_seconds("0"), 0);
}

TEST(WebDepthExRetryAfter, CapsHigh) {
    EXPECT_EQ(parse_retry_after_seconds("99999"), 3600);
}

TEST(WebDepthExRetryAfter, RejectsInvalid) {
    EXPECT_FALSE(parse_retry_after_seconds("abc").has_value());
    EXPECT_FALSE(parse_retry_after_seconds("").has_value());
    EXPECT_FALSE(
        parse_retry_after_seconds("Wed, 21 Oct 2015 07:28:00 GMT").has_value());
}

TEST(WebDepthExUA, ReturnsNonEmpty) {
    auto ua = default_user_agent();
    EXPECT_NE(ua.find("Hermes"), std::string::npos);
}

TEST(WebDepthExPrompt, ChunkPrefix) {
    auto s = chunk_prompt_prefix("T", "https://x", 0, 3);
    EXPECT_NE(s.find("Title: T"), std::string::npos);
    EXPECT_NE(s.find("Source: https://x"), std::string::npos);
    EXPECT_NE(s.find("chunk 1 of 3"), std::string::npos);
}

TEST(WebDepthExPrompt, ChunkPrefixEmptyFields) {
    auto s = chunk_prompt_prefix("", "", 2, 5);
    EXPECT_EQ(s.find("Title:"), std::string::npos);
    EXPECT_EQ(s.find("Source:"), std::string::npos);
    EXPECT_NE(s.find("chunk 3 of 5"), std::string::npos);
}

TEST(WebDepthExPrompt, SingleShotPrefix) {
    auto s = single_shot_prompt_prefix("T", "https://x");
    EXPECT_NE(s.find("Title: T"), std::string::npos);
    EXPECT_NE(s.find("Source: https://x"), std::string::npos);
}

TEST(WebDepthExPrompt, SingleShotEmptyFields) {
    EXPECT_EQ(single_shot_prompt_prefix("", ""), "");
}
