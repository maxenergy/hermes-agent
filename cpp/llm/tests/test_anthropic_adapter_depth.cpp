// Tests for anthropic_adapter_depth — message/tool conversion, kwargs
// builder, response normalisation, OAuth helpers, error taxonomy.
#include "hermes/llm/anthropic_adapter_depth.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace hermes::llm;

TEST(AnthropicAdapterDepth, MaxOutputLimits) {
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-opus-4-6"),         128000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-opus-4.6"),         128000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-sonnet-4-6"),        64000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-sonnet-4-5-20250929"),64000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-haiku-4-5"),         64000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-3-5-sonnet-20241022"), 8192);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-3-7-sonnet"),       128000);
    EXPECT_EQ(get_anthropic_max_output_tokens("claude-3-opus-20240229"),    4096);
    EXPECT_EQ(get_anthropic_max_output_tokens("minimax-m2.7"),          131072);
    EXPECT_EQ(get_anthropic_max_output_tokens("unknown-model-9001"),     128000);
}

TEST(AnthropicAdapterDepth, NormalizeModelName) {
    EXPECT_EQ(normalize_anthropic_model_name("anthropic/claude-opus-4.6"),
              "claude-opus-4-6");
    EXPECT_EQ(normalize_anthropic_model_name("ANTHROPIC/claude-sonnet-4-5"),
              "claude-sonnet-4-5");
    EXPECT_EQ(normalize_anthropic_model_name("claude-opus-4.6"),
              "claude-opus-4-6");
    EXPECT_EQ(normalize_anthropic_model_name("qwen3.5-plus", true),
              "qwen3.5-plus");
    EXPECT_EQ(normalize_anthropic_model_name("qwen3.5-plus", false),
              "qwen3-5-plus");
}

TEST(AnthropicAdapterDepth, SanitizeToolId) {
    EXPECT_EQ(sanitize_anthropic_tool_id("toolu_01ABC"), "toolu_01ABC");
    EXPECT_EQ(sanitize_anthropic_tool_id("call.123"),    "call_123");
    EXPECT_EQ(sanitize_anthropic_tool_id(""),            "tool_0");
    EXPECT_EQ(sanitize_anthropic_tool_id("a b/c"),       "a_b_c");
    EXPECT_EQ(sanitize_anthropic_tool_id("x-y_z-9"),     "x-y_z-9");
}

TEST(AnthropicAdapterDepth, ConvertToolsShape) {
    json tools = json::parse(R"([
        {"type":"function","function":{"name":"a","description":"d","parameters":{"type":"object","properties":{}}}},
        {"type":"function","function":{"name":"b"}}
    ])");
    json out = convert_openai_tools_to_anthropic(tools);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0]["name"], "a");
    EXPECT_EQ(out[0]["description"], "d");
    EXPECT_TRUE(out[0]["input_schema"].is_object());
    EXPECT_EQ(out[1]["name"], "b");
    EXPECT_TRUE(out[1]["input_schema"].is_object());
    EXPECT_EQ(out[1]["input_schema"]["type"], "object");
}

TEST(AnthropicAdapterDepth, ImageSourceFromUrl) {
    EXPECT_EQ(image_source_from_openai_url("https://x.com/a.png")["type"], "url");
    EXPECT_EQ(image_source_from_openai_url("  https://x.com/a.png  ")["url"],
              "https://x.com/a.png");
    json d = image_source_from_openai_url("data:image/png;base64,ABCD");
    EXPECT_EQ(d["type"], "base64");
    EXPECT_EQ(d["media_type"], "image/png");
    EXPECT_EQ(d["data"], "ABCD");
    json d2 = image_source_from_openai_url("data:image/gif,XYZ");
    EXPECT_EQ(d2["media_type"], "image/gif");
    EXPECT_EQ(image_source_from_openai_url("")["type"], "url");
}

TEST(AnthropicAdapterDepth, ContentPartConversion) {
    EXPECT_EQ(convert_openai_content_part("hello")["text"], "hello");
    json t = convert_openai_content_part(json{
        {"type","input_text"},{"text","hi"},
        {"cache_control",{{"type","ephemeral"}}}});
    EXPECT_EQ(t["type"], "text");
    EXPECT_EQ(t["text"], "hi");
    EXPECT_TRUE(t.contains("cache_control"));

    json img = convert_openai_content_part(json{
        {"type","image_url"},
        {"image_url",{{"url","data:image/png;base64,QUI="}}}});
    EXPECT_EQ(img["type"], "image");
    EXPECT_EQ(img["source"]["type"], "base64");
}

TEST(AnthropicAdapterDepth, SystemExtraction) {
    json msgs = json::parse(R"([
        {"role":"system","content":"Be helpful."},
        {"role":"user","content":"hi"}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    EXPECT_EQ(c.system, "Be helpful.");
    EXPECT_EQ(c.messages.size(), 1u);
    EXPECT_EQ(c.messages[0]["role"], "user");
}

TEST(AnthropicAdapterDepth, SystemCacheControlArray) {
    json msgs = json::parse(R"([
        {"role":"system","content":[
            {"type":"text","text":"base"},
            {"type":"text","text":"ctx","cache_control":{"type":"ephemeral"}}
        ]},
        {"role":"user","content":"hi"}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    ASSERT_TRUE(c.system.is_array());
    EXPECT_EQ(c.system.size(), 2u);
    EXPECT_TRUE(c.system[1].contains("cache_control"));
}

TEST(AnthropicAdapterDepth, AssistantToolCallConversion) {
    json msgs = json::parse(R"([
        {"role":"user","content":"x"},
        {"role":"assistant","content":"ok","tool_calls":[
            {"id":"call_1","type":"function","function":{"name":"f","arguments":"{\"a\":1}"}}
        ]},
        {"role":"tool","tool_call_id":"call_1","content":"42"}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    ASSERT_EQ(c.messages.size(), 3u);
    EXPECT_EQ(c.messages[1]["role"], "assistant");
    bool has_tool_use = false;
    for (const auto& b : c.messages[1]["content"]) {
        if (b["type"] == "tool_use") {
            has_tool_use = true;
            EXPECT_EQ(b["name"], "f");
            EXPECT_EQ(b["input"]["a"], 1);
        }
    }
    EXPECT_TRUE(has_tool_use);
    EXPECT_EQ(c.messages[2]["role"], "user");
    EXPECT_EQ(c.messages[2]["content"][0]["type"], "tool_result");
    EXPECT_EQ(c.messages[2]["content"][0]["tool_use_id"], "call_1");
}

TEST(AnthropicAdapterDepth, OrphanedToolUseStripped) {
    json msgs = json::parse(R"([
        {"role":"user","content":"x"},
        {"role":"assistant","content":"ok","tool_calls":[
            {"id":"call_1","type":"function","function":{"name":"f","arguments":"{}"}}
        ]}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    // assistant content should have the orphan tool_use removed; inserted placeholder text.
    bool found_text_placeholder = false;
    for (const auto& b : c.messages[1]["content"]) {
        if (b.value("type","") == "text" &&
            b.value("text","") == "(tool call removed)") {
            found_text_placeholder = true;
        }
        EXPECT_NE(b.value("type",""), "tool_use");
    }
    EXPECT_TRUE(found_text_placeholder || c.messages[1]["content"].size() > 0);
}

TEST(AnthropicAdapterDepth, ConsecutiveToolResultsMerge) {
    json msgs = json::parse(R"([
        {"role":"assistant","content":"","tool_calls":[
            {"id":"c1","type":"function","function":{"name":"f","arguments":"{}"}},
            {"id":"c2","type":"function","function":{"name":"g","arguments":"{}"}}
        ]},
        {"role":"tool","tool_call_id":"c1","content":"a"},
        {"role":"tool","tool_call_id":"c2","content":"b"}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    // Second-to-last should be the assistant, last a user with two tool_results.
    ASSERT_GE(c.messages.size(), 2u);
    const auto& last = c.messages.back();
    EXPECT_EQ(last["role"], "user");
    ASSERT_TRUE(last["content"].is_array());
    EXPECT_EQ(last["content"].size(), 2u);
    EXPECT_EQ(last["content"][0]["type"], "tool_result");
    EXPECT_EQ(last["content"][1]["type"], "tool_result");
}

TEST(AnthropicAdapterDepth, RoleAlternationMerge) {
    json msgs = json::parse(R"([
        {"role":"user","content":"a"},
        {"role":"user","content":"b"}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs);
    ASSERT_EQ(c.messages.size(), 1u);
    // merged content: "a\nb"
    if (c.messages[0]["content"].is_string()) {
        EXPECT_NE(c.messages[0]["content"].get<std::string>().find("a"),
                  std::string::npos);
        EXPECT_NE(c.messages[0]["content"].get<std::string>().find("b"),
                  std::string::npos);
    } else {
        EXPECT_TRUE(c.messages[0]["content"].is_array());
    }
}

TEST(AnthropicAdapterDepth, ThinkingBlockStripOnThirdParty) {
    json msgs = json::parse(R"([
        {"role":"user","content":"x"},
        {"role":"assistant",
         "content":"ok",
         "reasoning_details":[{"type":"thinking","thinking":"hm","signature":"sig"}]}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs, "https://api.minimax.chat/v1");
    bool any_thinking = false;
    for (const auto& b : c.messages[1]["content"]) {
        if (b.is_object() && (b.value("type","")=="thinking" ||
                              b.value("type","")=="redacted_thinking")) {
            any_thinking = true;
        }
    }
    EXPECT_FALSE(any_thinking);
}

TEST(AnthropicAdapterDepth, ThinkingKeptOnLatestNative) {
    json msgs = json::parse(R"([
        {"role":"user","content":"x"},
        {"role":"assistant",
         "content":"ok",
         "reasoning_details":[{"type":"thinking","thinking":"hm","signature":"sig"}]}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs, "https://api.anthropic.com");
    bool has_signed = false;
    for (const auto& b : c.messages[1]["content"]) {
        if (b.is_object() && b.value("type","")=="thinking") has_signed = true;
    }
    EXPECT_TRUE(has_signed);
}

TEST(AnthropicAdapterDepth, UnsignedThinkingDowngradedToText) {
    // No signature → downgrade to text.
    json msgs = json::parse(R"([
        {"role":"user","content":"x"},
        {"role":"assistant",
         "content":"ok",
         "reasoning_details":[{"type":"thinking","thinking":"reasoning text"}]}
    ])");
    auto c = convert_openai_messages_to_anthropic(msgs, "https://api.anthropic.com");
    bool found_text = false;
    for (const auto& b : c.messages[1]["content"]) {
        if (b.value("type","") == "text" &&
            b.value("text","").find("reasoning text") != std::string::npos) {
            found_text = true;
        }
        EXPECT_NE(b.value("type",""), "thinking");
    }
    EXPECT_TRUE(found_text);
}

TEST(AnthropicAdapterDepth, BuildKwargsBasic) {
    AnthropicBuildOptions o;
    o.model = "anthropic/claude-opus-4.6";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.max_tokens = 1024;
    json r = build_anthropic_kwargs(o);
    EXPECT_EQ(r["model"], "claude-opus-4-6");
    EXPECT_EQ(r["max_tokens"], 1024);
    EXPECT_TRUE(r["messages"].is_array());
}

TEST(AnthropicAdapterDepth, BuildKwargsToolChoiceMapping) {
    AnthropicBuildOptions o;
    o.model = "claude-sonnet-4-5";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.tools = json::parse(R"([{"type":"function","function":{"name":"f"}}])");
    o.tool_choice = "required";
    json r1 = build_anthropic_kwargs(o);
    EXPECT_EQ(r1["tool_choice"]["type"], "any");

    o.tool_choice = "none";
    json r2 = build_anthropic_kwargs(o);
    EXPECT_FALSE(r2.contains("tools"));

    o.tool_choice = "my_tool";
    json r3 = build_anthropic_kwargs(o);
    EXPECT_EQ(r3["tool_choice"]["type"], "tool");
    EXPECT_EQ(r3["tool_choice"]["name"], "my_tool");

    o.tool_choice = "auto";
    json r4 = build_anthropic_kwargs(o);
    EXPECT_EQ(r4["tool_choice"]["type"], "auto");
}

TEST(AnthropicAdapterDepth, BuildKwargsAdaptiveThinking) {
    AnthropicBuildOptions o;
    o.model = "claude-opus-4-6";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.reasoning_config = json{{"enabled", true}, {"effort", "high"}};
    json r = build_anthropic_kwargs(o);
    EXPECT_EQ(r["thinking"]["type"], "adaptive");
    EXPECT_EQ(r["output_config"]["effort"], "high");
}

TEST(AnthropicAdapterDepth, BuildKwargsManualThinkingClampsTemp) {
    AnthropicBuildOptions o;
    o.model = "claude-sonnet-4-5";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.max_tokens = 1024;
    o.reasoning_config = json{{"enabled", true}, {"effort", "high"}};
    json r = build_anthropic_kwargs(o);
    EXPECT_EQ(r["thinking"]["type"], "enabled");
    EXPECT_EQ(r["thinking"]["budget_tokens"], 16000);
    EXPECT_EQ(r["temperature"], 1);
    EXPECT_GE(r["max_tokens"].get<int>(), 16000 + 4096);
}

TEST(AnthropicAdapterDepth, BuildKwargsHaikuNoThinking) {
    AnthropicBuildOptions o;
    o.model = "claude-haiku-4-5";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.reasoning_config = json{{"enabled", true}, {"effort", "high"}};
    json r = build_anthropic_kwargs(o);
    EXPECT_FALSE(r.contains("thinking"));
}

TEST(AnthropicAdapterDepth, BuildKwargsOAuthTransforms) {
    AnthropicBuildOptions o;
    o.model = "claude-opus-4-6";
    o.messages = json::parse(R"([
        {"role":"system","content":"You are Hermes Agent powered by Nous Research."},
        {"role":"user","content":"hi"}
    ])");
    o.tools = json::parse(R"([{"type":"function","function":{"name":"read_file"}}])");
    o.is_oauth = true;
    json r = build_anthropic_kwargs(o);
    // System was elevated to array, with Claude Code prefix first.
    ASSERT_TRUE(r["system"].is_array());
    EXPECT_NE(r["system"][0]["text"].get<std::string>().find("Claude Code"),
              std::string::npos);
    // Tool name should be prefixed.
    EXPECT_EQ(r["tools"][0]["name"], "mcp_read_file");
    // Sanitize — no "Hermes Agent" / "Nous Research" remain.
    bool found_nous = false, found_hermes = false;
    for (const auto& b : r["system"]) {
        const std::string t = b.value("text","");
        if (t.find("Nous Research") != std::string::npos) found_nous = true;
        if (t.find("Hermes Agent") != std::string::npos) found_hermes = true;
    }
    EXPECT_FALSE(found_nous);
    EXPECT_FALSE(found_hermes);
}

TEST(AnthropicAdapterDepth, BuildKwargsFastModeOnlyNative) {
    AnthropicBuildOptions o;
    o.model = "claude-opus-4-6";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.fast_mode = true;
    o.base_url = "https://api.anthropic.com";
    json r = build_anthropic_kwargs(o);
    EXPECT_EQ(r["speed"], "fast");
    ASSERT_TRUE(r.contains("extra_headers"));
    const std::string hdr = r["extra_headers"]["anthropic-beta"].get<std::string>();
    EXPECT_NE(hdr.find("fast-mode-2026-02-01"), std::string::npos);

    o.base_url = "https://api.minimax.chat";
    json r2 = build_anthropic_kwargs(o);
    EXPECT_FALSE(r2.contains("speed"));
}

TEST(AnthropicAdapterDepth, BuildKwargsClampsOutputToContext) {
    AnthropicBuildOptions o;
    o.model = "claude-opus-4-6";
    o.messages = json::parse(R"([{"role":"user","content":"hi"}])");
    o.context_length = 4000;
    json r = build_anthropic_kwargs(o);
    EXPECT_LE(r["max_tokens"].get<int>(), 3999);
}

TEST(AnthropicAdapterDepth, NormalizeResponse) {
    json resp = json::parse(R"({
        "content":[
            {"type":"thinking","thinking":"let me think"},
            {"type":"text","text":"Hello."},
            {"type":"tool_use","id":"tu1","name":"mcp_read","input":{"path":"/x"}}
        ],
        "stop_reason":"tool_use",
        "usage":{"input_tokens":10,"output_tokens":5}
    })");
    auto n = normalize_anthropic_response(resp, /*strip_tool_prefix=*/true);
    ASSERT_TRUE(n.content.has_value());
    EXPECT_EQ(*n.content, "Hello.");
    ASSERT_TRUE(n.tool_calls.is_array());
    EXPECT_EQ(n.tool_calls[0]["function"]["name"], "read");
    ASSERT_TRUE(n.reasoning.has_value());
    EXPECT_NE(n.reasoning->find("let me think"), std::string::npos);
    EXPECT_EQ(n.finish_reason, "tool_calls");
    EXPECT_EQ(n.usage["input_tokens"], 10);
}

TEST(AnthropicAdapterDepth, NormalizeResponseStopReasonMapping) {
    json r1 = json{{"content", json::array()}, {"stop_reason", "end_turn"}};
    EXPECT_EQ(normalize_anthropic_response(r1).finish_reason, "stop");
    json r2 = json{{"content", json::array()}, {"stop_reason", "max_tokens"}};
    EXPECT_EQ(normalize_anthropic_response(r2).finish_reason, "length");
    json r3 = json{{"content", json::array()}, {"stop_reason", "stop_sequence"}};
    EXPECT_EQ(normalize_anthropic_response(r3).finish_reason, "stop");
}

TEST(AnthropicAdapterDepth, OAuthTokenDetection) {
    EXPECT_FALSE(is_anthropic_oauth_token(""));
    EXPECT_FALSE(is_anthropic_oauth_token("sk-ant-api03-AAA"));
    EXPECT_TRUE(is_anthropic_oauth_token("sk-ant-oat-01-ABC"));
    EXPECT_TRUE(is_anthropic_oauth_token("eyJhbGciOiJSUzI1NiJ9.foo.bar"));
    EXPECT_FALSE(is_anthropic_oauth_token("gsk_openai_key"));
}

TEST(AnthropicAdapterDepth, RequiresBearerAuth) {
    EXPECT_TRUE(requires_bearer_auth("https://api.anthropic.com",
                                     "sk-ant-oat-01-ABC"));
    EXPECT_FALSE(requires_bearer_auth("https://api.anthropic.com",
                                      "sk-ant-api03-XYZ"));
    EXPECT_FALSE(requires_bearer_auth("https://api.minimax.chat",
                                      "sk-ant-oat-01-ABC"));
}

TEST(AnthropicAdapterDepth, OAuthRefreshBody) {
    const std::string form = build_oauth_refresh_body("rt-abc", /*use_json=*/false);
    EXPECT_NE(form.find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(form.find("refresh_token=rt-abc"),     std::string::npos);
    EXPECT_NE(form.find("client_id="),               std::string::npos);

    const std::string js = build_oauth_refresh_body("rt-abc", /*use_json=*/true);
    json parsed = json::parse(js);
    EXPECT_EQ(parsed["grant_type"], "refresh_token");
    EXPECT_EQ(parsed["refresh_token"], "rt-abc");
}

TEST(AnthropicAdapterDepth, ClaudeCodeTokenValidity) {
    EXPECT_FALSE(is_claude_code_token_valid(json::object()));
    EXPECT_FALSE(is_claude_code_token_valid(json{{"access_token",""}}));
    EXPECT_TRUE (is_claude_code_token_valid(json{{"access_token","abc"}}));
    const int64_t past = 1000;
    EXPECT_FALSE(is_claude_code_token_valid(
        json{{"access_token","abc"},{"expires_at", past}}));
    const int64_t far_future = 9999999999999LL;
    EXPECT_TRUE (is_claude_code_token_valid(
        json{{"access_token","abc"},{"expires_at", far_future}}));
}

TEST(AnthropicAdapterDepth, PkceGeneration) {
    // Deterministic with seed
    PkcePair a = generate_pkce_pair(42);
    PkcePair b = generate_pkce_pair(42);
    EXPECT_EQ(a.code_verifier, b.code_verifier);
    EXPECT_EQ(a.code_challenge, b.code_challenge);
    EXPECT_EQ(a.method, "S256");
    EXPECT_GE(a.code_verifier.size(), 43u);
    EXPECT_LE(a.code_verifier.size(), 128u);
    // code_challenge base64url-no-pad of sha256 (32 bytes → 43 chars).
    EXPECT_EQ(a.code_challenge.size(), 43u);
    // Different seed → different pair.
    PkcePair c = generate_pkce_pair(43);
    EXPECT_NE(a.code_verifier, c.code_verifier);
}

TEST(AnthropicAdapterDepth, ThinkingStrategyPerUrl) {
    EXPECT_EQ(thinking_strategy_for_base_url("https://api.anthropic.com"),
              ThinkingStrategy::KeepLatestOnly);
    EXPECT_EQ(thinking_strategy_for_base_url("https://api.minimax.chat"),
              ThinkingStrategy::StripAll);
}

TEST(AnthropicAdapterDepth, ErrorClassification) {
    EXPECT_EQ(classify_anthropic_error(400, "Invalid signature in thinking block"),
              AnthropicErrorKind::InvalidSignature);
    EXPECT_EQ(classify_anthropic_error(400, "max_tokens: 65536 > 8192"),
              AnthropicErrorKind::MaxTokensTooLarge);
    EXPECT_EQ(classify_anthropic_error(400, "prompt is too long for the context length"),
              AnthropicErrorKind::ContextTooLong);
    EXPECT_EQ(classify_anthropic_error(400, "bad field foo"),
              AnthropicErrorKind::InvalidRequest);
    EXPECT_EQ(classify_anthropic_error(401, ""), AnthropicErrorKind::Authentication);
    EXPECT_EQ(classify_anthropic_error(403, ""), AnthropicErrorKind::PermissionDenied);
    EXPECT_EQ(classify_anthropic_error(404, ""), AnthropicErrorKind::NotFound);
    EXPECT_EQ(classify_anthropic_error(413, ""), AnthropicErrorKind::RequestTooLarge);
    EXPECT_EQ(classify_anthropic_error(429, ""), AnthropicErrorKind::RateLimit);
    EXPECT_EQ(classify_anthropic_error(529, ""), AnthropicErrorKind::Overloaded);
    EXPECT_EQ(classify_anthropic_error(500, ""), AnthropicErrorKind::ServerError);
    EXPECT_EQ(classify_anthropic_error(502, ""), AnthropicErrorKind::ServerError);
    EXPECT_EQ(classify_anthropic_error(503, ""), AnthropicErrorKind::ServerError);
    EXPECT_EQ(classify_anthropic_error(504, ""), AnthropicErrorKind::GatewayTimeout);
}

TEST(AnthropicAdapterDepth, RetryableClasses) {
    EXPECT_TRUE (anthropic_error_is_retryable(AnthropicErrorKind::RateLimit));
    EXPECT_TRUE (anthropic_error_is_retryable(AnthropicErrorKind::Overloaded));
    EXPECT_TRUE (anthropic_error_is_retryable(AnthropicErrorKind::ServerError));
    EXPECT_TRUE (anthropic_error_is_retryable(AnthropicErrorKind::GatewayTimeout));
    EXPECT_FALSE(anthropic_error_is_retryable(AnthropicErrorKind::InvalidRequest));
    EXPECT_FALSE(anthropic_error_is_retryable(AnthropicErrorKind::Authentication));
    EXPECT_FALSE(anthropic_error_is_retryable(AnthropicErrorKind::MaxTokensTooLarge));
}

TEST(AnthropicAdapterDepth, ParseAvailableMaxTokens) {
    auto a = parse_available_max_tokens("max_tokens: 65536 > 8192, which is the maximum");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 8192);
    auto b = parse_available_max_tokens("maximum allowed is 4096 tokens");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, 4096);
    auto c = parse_available_max_tokens("no number here");
    EXPECT_FALSE(c.has_value());
}
