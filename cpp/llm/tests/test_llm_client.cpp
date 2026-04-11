#include "hermes/llm/anthropic_client.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/llm/openrouter_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using hermes::llm::AnthropicClient;
using hermes::llm::ApiError;
using hermes::llm::CompletionRequest;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::Message;
using hermes::llm::OpenAIClient;
using hermes::llm::OpenRouterClient;
using hermes::llm::PromptCacheOptions;
using hermes::llm::Role;
using json = nlohmann::json;

namespace {

Message user_msg(const std::string& text) {
    Message m;
    m.role = Role::User;
    m.content_text = text;
    return m;
}

}  // namespace

TEST(LlmClient, OpenAiParsesToolCalls) {
    FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "chatcmpl-1"},
        {"choices", json::array({{
            {"index", 0},
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", json::array({{
                    {"id", "call_abc"},
                    {"type", "function"},
                    {"function", {
                        {"name", "read_file"},
                        {"arguments", "{\"path\":\"/tmp/a\"}"},
                    }},
                }})},
            }},
        }})},
        {"usage", {
            {"prompt_tokens", 100},
            {"completion_tokens", 50},
        }},
    }.dump();
    fake.enqueue_response(resp);

    OpenAIClient client(&fake, "sk-fake");
    CompletionRequest req;
    req.model = "gpt-4o";
    req.messages = {user_msg("list /tmp")};
    const auto out = client.complete(req);

    EXPECT_EQ(out.finish_reason, "tool_calls");
    ASSERT_EQ(out.assistant_message.tool_calls.size(), 1u);
    EXPECT_EQ(out.assistant_message.tool_calls[0].name, "read_file");
    EXPECT_EQ(out.assistant_message.tool_calls[0].arguments["path"], "/tmp/a");
    EXPECT_EQ(out.usage.input_tokens, 100);
    EXPECT_EQ(out.usage.output_tokens, 50);

    ASSERT_EQ(fake.requests().size(), 1u);
    const auto& req_seen = fake.requests()[0];
    EXPECT_NE(req_seen.url.find("/chat/completions"), std::string::npos);
    EXPECT_EQ(req_seen.headers.at("Authorization"), "Bearer sk-fake");
}

TEST(LlmClient, OpenAiThrowsApiErrorOn429) {
    FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 429;
    resp.body = "{\"error\":{\"message\":\"rate limit\"}}";
    fake.enqueue_response(resp);

    OpenAIClient client(&fake, "sk");
    CompletionRequest req;
    req.model = "gpt-4o";
    req.messages = {user_msg("hi")};

    try {
        client.complete(req);
        FAIL() << "expected ApiError";
    } catch (const ApiError& e) {
        EXPECT_EQ(e.status, 429);
        EXPECT_EQ(e.provider, "openai");
        EXPECT_NE(e.body.find("rate limit"), std::string::npos);
    }
}

TEST(LlmClient, AnthropicParsesContentAndUsage) {
    FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"id", "msg_1"},
        {"type", "message"},
        {"role", "assistant"},
        {"stop_reason", "end_turn"},
        {"content", json::array({
            {{"type", "text"}, {"text", "hello back"}},
        })},
        {"usage", {
            {"input_tokens", 80},
            {"output_tokens", 20},
            {"cache_read_input_tokens", 40},
            {"cache_creation_input_tokens", 10},
        }},
    }.dump();
    fake.enqueue_response(resp);

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-sonnet-4-6";
    req.messages = {user_msg("hello")};
    const auto out = client.complete(req);

    EXPECT_EQ(out.finish_reason, "end_turn");
    ASSERT_FALSE(out.assistant_message.content_blocks.empty());
    EXPECT_EQ(out.assistant_message.content_blocks[0].text, "hello back");
    EXPECT_EQ(out.usage.input_tokens, 80);
    EXPECT_EQ(out.usage.output_tokens, 20);
    EXPECT_EQ(out.usage.cache_read_input_tokens, 40);
    EXPECT_EQ(out.usage.cache_creation_input_tokens, 10);

    const auto& seen = fake.requests()[0];
    EXPECT_NE(seen.url.find("/messages"), std::string::npos);
    EXPECT_EQ(seen.headers.at("x-api-key"), "sk-ant");
    EXPECT_EQ(seen.headers.at("anthropic-version"), "2023-06-01");
}

TEST(LlmClient, AnthropicAppliesCacheControlWhenEnabled) {
    FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"role", "assistant"},
        {"content", json::array({{{"type", "text"}, {"text", "ok"}}})},
        {"stop_reason", "end_turn"},
        {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}},
    }.dump();
    fake.enqueue_response(resp);

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-sonnet-4-6";
    Message system_msg;
    system_msg.role = Role::System;
    system_msg.content_text = "you are helpful";
    req.messages = {system_msg, user_msg("hi")};
    req.cache.native_anthropic = true;

    client.complete(req);

    // Inspect the outgoing body.
    const auto& seen = fake.requests()[0];
    const auto body = json::parse(seen.body);
    ASSERT_TRUE(body.contains("system"));
    // system field is an array of blocks and the first block should
    // carry cache_control.
    ASSERT_TRUE(body["system"].is_array());
    ASSERT_FALSE(body["system"].empty());
    EXPECT_TRUE(body["system"][0].contains("cache_control"));
}

TEST(LlmClient, OpenRouterSendsRefererAndTitle) {
    FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = json{
        {"choices", json::array({{
            {"finish_reason", "stop"},
            {"message", {{"role", "assistant"}, {"content", "ok"}}},
        }})},
        {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 1}}},
    }.dump();
    fake.enqueue_response(resp);

    OpenRouterClient client(&fake, "sk-or", "https://example.com", "unit-test");
    CompletionRequest req;
    req.model = "meta-llama/llama-3.3-70b";
    req.messages = {user_msg("hi")};
    client.complete(req);

    const auto& seen = fake.requests()[0];
    EXPECT_EQ(seen.headers.at("HTTP-Referer"), "https://example.com");
    EXPECT_EQ(seen.headers.at("X-Title"), "unit-test");
    EXPECT_EQ(seen.headers.at("Authorization"), "Bearer sk-or");
    EXPECT_NE(seen.url.find("openrouter.ai"), std::string::npos);
}

TEST(LlmClient, StreamingThrowsNotImplemented) {
    FakeHttpTransport fake;
    OpenAIClient client(&fake, "sk");
    CompletionRequest req;
    req.model = "gpt-4o";
    req.stream = true;
    req.messages = {user_msg("hi")};
    EXPECT_THROW(client.complete(req), std::runtime_error);
}

TEST(LlmClient, FakeTransportThrowsWhenEmpty) {
    FakeHttpTransport fake;
    OpenAIClient client(&fake, "sk");
    CompletionRequest req;
    req.model = "gpt-4o";
    req.messages = {user_msg("hi")};
    EXPECT_THROW(client.complete(req), std::runtime_error);
}
