// Tests for gateway::api_server_depth — pure helpers ported from
// gateway/platforms/api_server.py.

#include <hermes/gateway/api_server_depth.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace asd = hermes::gateway::api_server_depth;
using nlohmann::json;

TEST(ApiServerDepth, OpenAiErrorShape) {
    const auto err = asd::openai_error("Body too large", "server_error",
                                         "messages", "body_too_large");
    ASSERT_TRUE(err.contains("error"));
    EXPECT_EQ(err["error"]["message"], "Body too large");
    EXPECT_EQ(err["error"]["type"], "server_error");
    EXPECT_EQ(err["error"]["param"], "messages");
    EXPECT_EQ(err["error"]["code"], "body_too_large");

    const auto defaults = asd::openai_error("oops");
    EXPECT_EQ(defaults["error"]["type"], "invalid_request_error");
    EXPECT_TRUE(defaults["error"]["param"].is_null());
    EXPECT_TRUE(defaults["error"]["code"].is_null());
}

TEST(ApiServerDepth, FingerprintIsDeterministic) {
    json body1 = {{"model", "gpt"}, {"stream", true}, {"messages", {"x"}}};
    json body2 = {{"messages", {"x"}}, {"model", "gpt"}, {"stream", true}};
    const std::vector<std::string> keys{"model", "stream", "messages"};
    EXPECT_EQ(asd::make_request_fingerprint(body1, keys),
              asd::make_request_fingerprint(body2, keys));
    // Missing keys are treated as null — distinct from the value "null"
    // only insofar as Python's dict.get returns None.
    json body3 = {{"model", "gpt"}};
    EXPECT_NE(asd::make_request_fingerprint(body1, keys),
              asd::make_request_fingerprint(body3, keys));
}

TEST(ApiServerDepth, SessionIdStable) {
    const auto id1 = asd::derive_chat_session_id("sys", "hello");
    const auto id2 = asd::derive_chat_session_id("sys", "hello");
    const auto id3 = asd::derive_chat_session_id("other", "hello");
    EXPECT_EQ(id1, id2);
    EXPECT_NE(id1, id3);
    EXPECT_EQ(id1.rfind("api-", 0), 0u);
    EXPECT_EQ(id1.size(), 4u + 16u);
}

TEST(ApiServerDepth, CorsOriginsParse) {
    const auto a = asd::parse_cors_origins(std::string_view{"http://a, http://b ,,\t"});
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0], "http://a");
    EXPECT_EQ(a[1], "http://b");

    const auto b = asd::parse_cors_origins(json::array({"x", "y", ""}));
    ASSERT_EQ(b.size(), 2u);

    const auto c = asd::parse_cors_origins(json(""));
    EXPECT_TRUE(c.empty());
}

TEST(ApiServerDepth, OriginAllowed) {
    const std::vector<std::string> list{"http://a", "http://b"};
    EXPECT_TRUE(asd::origin_allowed(list, ""));  // non-browser
    EXPECT_TRUE(asd::origin_allowed(list, "http://a"));
    EXPECT_FALSE(asd::origin_allowed(list, "http://c"));
    EXPECT_FALSE(asd::origin_allowed({}, "http://a"));
    EXPECT_TRUE(asd::origin_allowed({"*"}, "http://anything"));
}

TEST(ApiServerDepth, CorsHeaders) {
    const std::vector<std::string> list{"http://a"};
    const auto headers = asd::cors_headers_for_origin(list, "http://a");
    ASSERT_TRUE(headers.has_value());
    EXPECT_EQ(headers->at("Access-Control-Allow-Origin"), "http://a");
    EXPECT_EQ(headers->at("Vary"), "Origin");

    EXPECT_FALSE(asd::cors_headers_for_origin(list, "http://bad").has_value());

    const auto wild =
        asd::cors_headers_for_origin({"*"}, "http://anything");
    ASSERT_TRUE(wild.has_value());
    EXPECT_EQ(wild->at("Access-Control-Allow-Origin"), "*");
}

TEST(ApiServerDepth, ModelNameFallback) {
    EXPECT_EQ(asd::resolve_model_name("gpt-foo"), "gpt-foo");
    EXPECT_EQ(asd::resolve_model_name("  gpt  "), "gpt");
    EXPECT_EQ(asd::resolve_model_name("", "coder"), "coder");
    EXPECT_EQ(asd::resolve_model_name("", "default"), "hermes-agent");
    EXPECT_EQ(asd::resolve_model_name("", "custom"), "hermes-agent");
    EXPECT_EQ(asd::resolve_model_name("", ""), "hermes-agent");
    EXPECT_EQ(asd::resolve_model_name("", "", "mine"), "mine");
}

TEST(ApiServerDepth, IdempotencyCacheHits) {
    asd::IdempotencyCache cache{3, std::chrono::seconds(60)};
    std::atomic<int> calls{0};
    auto compute = [&]() {
        ++calls;
        return json{{"x", calls.load()}};
    };

    const auto first =
        cache.get_or_compute("k", "fp", compute);
    EXPECT_EQ(first["x"], 1);
    const auto second =
        cache.get_or_compute("k", "fp", compute);
    EXPECT_EQ(second["x"], 1);  // cached
    EXPECT_EQ(calls.load(), 1);

    // Different fingerprint → recompute, overwriting.
    const auto third =
        cache.get_or_compute("k", "fp2", compute);
    EXPECT_EQ(third["x"], 2);
    EXPECT_EQ(calls.load(), 2);
}

TEST(ApiServerDepth, IdempotencyCacheEvicts) {
    asd::IdempotencyCache cache{2, std::chrono::seconds(60)};
    cache.put("a", "fp", json{1});
    cache.put("b", "fp", json{2});
    cache.put("c", "fp", json{3});
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_FALSE(cache.peek("a", "fp").has_value());
    EXPECT_TRUE(cache.peek("b", "fp").has_value());
    EXPECT_TRUE(cache.peek("c", "fp").has_value());
}

TEST(ApiServerDepth, IdempotencyCacheExpiresByClock) {
    auto now = std::chrono::steady_clock::now();
    asd::IdempotencyCache::Clock clock;
    clock.now = [&]() { return now; };
    asd::IdempotencyCache cache{10, std::chrono::seconds(5), clock};
    cache.put("k", "fp", json{"v"});
    EXPECT_TRUE(cache.peek("k", "fp").has_value());
    now += std::chrono::seconds(10);
    EXPECT_FALSE(cache.peek("k", "fp").has_value());
    EXPECT_EQ(cache.size(), 0u);
}

TEST(ApiServerDepth, ExtractOutputItems) {
    json result = {
        {"messages",
          {
              {{"role", "assistant"},
                {"tool_calls", json::array({
                                     {{"id", "call_1"},
                                       {"function",
                                         {{"name", "lookup"},
                                          {"arguments", "{\"q\":1}"}}}},
                                 })}},
              {{"role", "tool"},
                {"tool_call_id", "call_1"},
                {"content", "42"}},
          }},
        {"final_response", "The answer is 42."},
    };
    const auto items = asd::extract_output_items(result);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0]["type"], "function_call");
    EXPECT_EQ(items[0]["name"], "lookup");
    EXPECT_EQ(items[0]["call_id"], "call_1");
    EXPECT_EQ(items[1]["type"], "function_call_output");
    EXPECT_EQ(items[1]["output"], "42");
    EXPECT_EQ(items[2]["type"], "message");
    EXPECT_EQ(items[2]["content"][0]["text"], "The answer is 42.");
}

TEST(ApiServerDepth, ExtractOutputItemsEmptyFallback) {
    json result = {{"messages", json::array()}, {"final_response", ""}};
    const auto items = asd::extract_output_items(result);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0]["content"][0]["text"], "(No response generated)");
}

TEST(ApiServerDepth, BodyLimitClassifier) {
    using S = asd::BodyLimitStatus;
    EXPECT_EQ(asd::classify_body_length("", 1024), S::Ok);
    EXPECT_EQ(asd::classify_body_length("100", 1024), S::Ok);
    EXPECT_EQ(asd::classify_body_length("2000", 1024), S::TooLarge);
    EXPECT_EQ(asd::classify_body_length("abc", 1024), S::Invalid);
}

TEST(ApiServerDepth, BearerAuth) {
    EXPECT_TRUE(asd::check_bearer_auth("anything", ""));  // no key
    EXPECT_TRUE(asd::check_bearer_auth("Bearer  sk-abc ", "sk-abc"));
    EXPECT_FALSE(asd::check_bearer_auth("Bearer wrong", "sk-abc"));
    EXPECT_FALSE(asd::check_bearer_auth("", "sk-abc"));
    EXPECT_FALSE(asd::check_bearer_auth("Basic sk-abc", "sk-abc"));
}

TEST(ApiServerDepth, IdempotencyCacheRefresh) {
    // Re-inserting the same key with a new value must update without
    // exceeding the capacity limit.
    asd::IdempotencyCache cache{1, std::chrono::seconds(60)};
    cache.put("k", "fp1", json{1});
    cache.put("k", "fp2", json{2});
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.peek("k", "fp1").has_value());
    auto hit = cache.peek("k", "fp2");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, json{2});
}
