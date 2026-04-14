// Integration tests for the enhanced AnthropicClient depth features.
// Verifies that the new request-shaping and response-parsing features
// reach the wire.
#include "hermes/llm/anthropic_client.hpp"
#include "hermes/llm/anthropic_features.hpp"
#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hermes::llm;
using json = nlohmann::json;

namespace {

HttpTransport::Response ok_body(const json& body) {
    HttpTransport::Response r;
    r.status_code = 200;
    r.body = body.dump();
    return r;
}

Message user(const std::string& s) {
    Message m; m.role = Role::User; m.content_text = s; return m;
}

json basic_response_body(const std::string& text = "ok") {
    return json{
        {"id", "msg_1"},
        {"type", "message"},
        {"role", "assistant"},
        {"model", "claude-opus-4-6"},
        {"stop_reason", "end_turn"},
        {"content", json::array({
            {{"type", "text"}, {"text", text}}
        })},
        {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}},
    };
}

}  // namespace

TEST(AnthropicDepth, StopSequencesForwardedToWire) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.extra = json{{"stop_sequences", {"###", "END", "###"}}};
    client.complete(req);

    ASSERT_EQ(fake.requests().size(), 1u);
    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("stop_sequences"));
    ASSERT_EQ(body["stop_sequences"].size(), 2u);  // deduped
    EXPECT_EQ(body["stop_sequences"][0], "###");
    EXPECT_EQ(body["stop_sequences"][1], "END");
}

TEST(AnthropicDepth, TopPTopKServiceTier) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.extra = json{
        {"top_p", 0.95},
        {"top_k", 50},
        {"service_tier", "priority"},
    };
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    EXPECT_NEAR(body.value("top_p", 0.0), 0.95, 1e-9);
    EXPECT_EQ(body.value("top_k", 0), 50);
    EXPECT_EQ(body.value("service_tier", ""), "priority");
}

TEST(AnthropicDepth, ToolChoiceNoneDropsTools) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    ToolSchema t;
    t.name = "read_file";
    t.description = "read a file";
    t.parameters = json::object();
    req.tools = {t};
    req.extra = json{{"tool_choice", "none"}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
}

TEST(AnthropicDepth, ToolChoiceRequiredMapsToAny) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    ToolSchema t;
    t.name = "search";
    t.description = "search";
    t.parameters = json::object();
    req.tools = {t};
    req.extra = json{{"tool_choice", "required"}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body["tool_choice"].value("type", ""), "any");
}

TEST(AnthropicDepth, ThinkingEffortAdaptiveForOpus46) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.extra = json{{"thinking_effort", "high"}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("thinking"));
    EXPECT_EQ(body["thinking"].value("type", ""), "adaptive");
    ASSERT_TRUE(body.contains("output_config"));
    EXPECT_EQ(body["output_config"].value("effort", ""), "high");
}

TEST(AnthropicDepth, ThinkingEffortManualForOpus4) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-20250501";
    req.messages = {user("hi")};
    req.max_tokens = 2048;
    req.extra = json{{"thinking_effort", "medium"}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("thinking"));
    EXPECT_EQ(body["thinking"].value("type", ""), "enabled");
    EXPECT_EQ(body["thinking"].value("budget_tokens", 0), 8000);
    EXPECT_EQ(body.value("temperature", 0), 1);
    EXPECT_EQ(body.value("max_tokens", 0), 12096);
}

TEST(AnthropicDepth, OAuthUsesBearerAuth) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "oat-token-xyz");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.extra = json{{"is_oauth", true}};
    client.complete(req);

    const auto& hdrs = fake.requests()[0].headers;
    ASSERT_TRUE(hdrs.count("Authorization"));
    EXPECT_EQ(hdrs.at("Authorization"), "Bearer oat-token-xyz");
    EXPECT_FALSE(hdrs.count("x-api-key"));
}

TEST(AnthropicDepth, OAuthPrefixesToolNames) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "oat-token");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    ToolSchema t;
    t.name = "read_file";
    t.description = "read";
    t.parameters = json::object();
    req.tools = {t};
    req.extra = json{{"is_oauth", true}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body["tools"][0].value("name", ""), "mcp_read_file");
}

TEST(AnthropicDepth, ResponseStopReasonMapsToFinishReason) {
    FakeHttpTransport fake;
    auto resp = basic_response_body();
    resp["stop_reason"] = "max_tokens";
    fake.enqueue_response(ok_body(resp));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    auto out = client.complete(req);
    EXPECT_EQ(out.finish_reason, "length");
}

TEST(AnthropicDepth, ResponseExtractsReasoningText) {
    FakeHttpTransport fake;
    json resp = basic_response_body("final answer");
    resp["content"] = json::array({
        {{"type", "thinking"}, {"thinking", "step 1"}},
        {{"type", "thinking"}, {"thinking", "step 2"}},
        {{"type", "text"}, {"text", "final answer"}},
    });
    fake.enqueue_response(ok_body(resp));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    auto out = client.complete(req);

    ASSERT_TRUE(out.assistant_message.reasoning.has_value());
    EXPECT_EQ(*out.assistant_message.reasoning, "step 1\n\nstep 2");
}

TEST(AnthropicDepth, CacheOneHourAddsExtendedTtlBeta) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.cache.native_anthropic = true;
    req.cache.cache_ttl = "1h";
    client.complete(req);

    const auto& hdrs = fake.requests()[0].headers;
    ASSERT_TRUE(hdrs.count("anthropic-beta"));
    const auto& betas = hdrs.at("anthropic-beta");
    EXPECT_NE(betas.find("extended-cache-ttl-2025-04-11"), std::string::npos);
}

TEST(AnthropicDepth, OAuthSanitizesSystemPrompt) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "oat-x");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    Message sys; sys.role = Role::System;
    sys.content_text = "You are Hermes Agent built by Nous Research";
    req.messages = {sys, user("hi")};
    req.extra = json{{"is_oauth", true}};
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("system"));
    const auto dumped = body["system"].dump();
    EXPECT_EQ(dumped.find("Hermes Agent"), std::string::npos);
    EXPECT_EQ(dumped.find("Nous Research"), std::string::npos);
    EXPECT_NE(dumped.find("Claude Code"), std::string::npos);
    EXPECT_NE(dumped.find("Anthropic"), std::string::npos);
}

TEST(AnthropicDepth, ReasoningEffortIntegerMapsToThinking) {
    FakeHttpTransport fake;
    fake.enqueue_response(ok_body(basic_response_body()));

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.reasoning_effort = 3;  // "high"
    client.complete(req);

    auto body = json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("thinking"));
    EXPECT_EQ(body["thinking"].value("type", ""), "adaptive");
    EXPECT_EQ(body["output_config"].value("effort", ""), "high");
}

TEST(AnthropicDepth, StreamingThinkingDeltaAccumulates) {
    FakeHttpTransport fake;
    // Build a minimal SSE stream with a thinking_delta.
    std::string sse;
    sse += "event: message_start\n";
    sse += "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
           "\"type\":\"message\",\"role\":\"assistant\","
           "\"usage\":{\"input_tokens\":5,\"output_tokens\":0}}}\n\n";
    sse += "event: content_block_start\n";
    sse += "data: {\"type\":\"content_block_start\",\"index\":0,"
           "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n";
    sse += "event: content_block_delta\n";
    sse += "data: {\"type\":\"content_block_delta\",\"index\":0,"
           "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm \"}}\n\n";
    sse += "event: content_block_delta\n";
    sse += "data: {\"type\":\"content_block_delta\",\"index\":0,"
           "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"let me see\"}}\n\n";
    sse += "event: content_block_stop\n";
    sse += "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
    sse += "event: message_delta\n";
    sse += "data: {\"type\":\"message_delta\","
           "\"delta\":{\"stop_reason\":\"end_turn\"},"
           "\"usage\":{\"output_tokens\":3}}\n\n";
    sse += "event: message_stop\n";
    sse += "data: {\"type\":\"message_stop\"}\n\n";
    fake.enqueue_stream_response(sse);

    AnthropicClient client(&fake, "sk-ant");
    CompletionRequest req;
    req.model = "claude-opus-4-6";
    req.messages = {user("hi")};
    req.stream = true;
    auto out = client.complete(req);

    ASSERT_TRUE(out.assistant_message.reasoning.has_value());
    EXPECT_EQ(*out.assistant_message.reasoning, "hmm let me see");
    EXPECT_EQ(out.finish_reason, "stop");
}
