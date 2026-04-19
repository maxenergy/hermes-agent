// Unit tests for anthropic_features.hpp helpers.
#include "hermes/llm/anthropic_features.hpp"
#include "hermes/llm/message.hpp"
#include "hermes/llm/prompt_cache.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace hermes::llm;

TEST(AnthropicFeatures, ToolChoiceAutoMapping) {
    auto r = map_tool_choice_to_anthropic("auto");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r).value("type", ""), "auto");
}

TEST(AnthropicFeatures, ToolChoiceRequiredBecomesAny) {
    auto r = map_tool_choice_to_anthropic("required");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r).value("type", ""), "any");
}

TEST(AnthropicFeatures, ToolChoiceNoneReturnsNullopt) {
    auto r = map_tool_choice_to_anthropic("none");
    EXPECT_FALSE(r.has_value());
}

TEST(AnthropicFeatures, ToolChoiceSpecificName) {
    auto r = map_tool_choice_to_anthropic("search_code");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r).value("type", ""), "tool");
    EXPECT_EQ((*r).value("name", ""), "search_code");
}

TEST(AnthropicFeatures, ToolChoiceEmptyDefaultsToAuto) {
    auto r = map_tool_choice_to_anthropic("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r).value("type", ""), "auto");
}

TEST(AnthropicFeatures, ThinkingBudgetLevels) {
    EXPECT_EQ(thinking_budget_for_effort("minimal"), 2048);
    EXPECT_EQ(thinking_budget_for_effort("low"),     2048);
    EXPECT_EQ(thinking_budget_for_effort("medium"),  8000);
    EXPECT_EQ(thinking_budget_for_effort("high"),    16000);
    EXPECT_EQ(thinking_budget_for_effort("maximum"), 32000);
    EXPECT_EQ(thinking_budget_for_effort("unknown"), 8000);
}

TEST(AnthropicFeatures, AdaptiveEffortMapping) {
    // Per upstream Python commit 0517ac3e:
    //   minimal → low, low → low, medium → medium, high → high,
    //   xhigh → xhigh (on 4.7+, collapses to max on pre-4.7), max → max.
    EXPECT_EQ(std::string(map_adaptive_effort("minimal")), "low");
    EXPECT_EQ(std::string(map_adaptive_effort("low")),     "low");
    EXPECT_EQ(std::string(map_adaptive_effort("medium")),  "medium");
    EXPECT_EQ(std::string(map_adaptive_effort("high")),    "high");
    // max is now a distinct ceiling (was "high" pre-4.7).
    EXPECT_EQ(std::string(map_adaptive_effort("maximum")), "max");
    EXPECT_EQ(std::string(map_adaptive_effort("max")),     "max");
    // Legacy overload has no model — xhigh collapses to max.
    EXPECT_EQ(std::string(map_adaptive_effort("xhigh")),   "max");
}

TEST(AnthropicFeatures, AdaptiveEffortMapping_XhighOnlyOn47Plus) {
    // Upstream Python commit 63d06dd9: xhigh is a real level on 4.7+
    // but must collapse to max on pre-4.7 (which only exposes 4 levels).
    EXPECT_EQ(std::string(map_adaptive_effort("xhigh", "claude-opus-4-7")), "xhigh");
    EXPECT_EQ(std::string(map_adaptive_effort("xhigh", "claude-opus-4-6")), "max");
    EXPECT_EQ(std::string(map_adaptive_effort("xhigh", "claude-sonnet-4-6")), "max");
    EXPECT_EQ(std::string(map_adaptive_effort("high",  "claude-opus-4-7")), "high");
    EXPECT_EQ(std::string(map_adaptive_effort("max",   "claude-opus-4-6")), "max");
}

TEST(AnthropicFeatures, SupportsAdaptiveThinking) {
    EXPECT_TRUE(supports_adaptive_thinking("claude-opus-4-6"));
    EXPECT_TRUE(supports_adaptive_thinking("claude-sonnet-4-6-20251022"));
    // 4.7 joins the adaptive-thinking family.
    EXPECT_TRUE(supports_adaptive_thinking("claude-opus-4-7"));
    EXPECT_TRUE(supports_adaptive_thinking("claude-opus-4.7"));
    EXPECT_FALSE(supports_adaptive_thinking("claude-opus-4"));
    EXPECT_FALSE(supports_adaptive_thinking("claude-3-5-sonnet"));
}

TEST(AnthropicFeatures, ForbidsSamplingParamsOn47Only) {
    // 4.7+ rejects temperature / top_p / top_k.
    EXPECT_TRUE(forbids_sampling_params("claude-opus-4-7"));
    EXPECT_TRUE(forbids_sampling_params("claude-opus-4.7"));
    EXPECT_FALSE(forbids_sampling_params("claude-opus-4-6"));
    EXPECT_FALSE(forbids_sampling_params("claude-sonnet-4-6"));
    EXPECT_FALSE(forbids_sampling_params("claude-3-5-sonnet"));
}

TEST(AnthropicFeatures, BuildThinking47RequestsSummarizedDisplay) {
    // Opus 4.7 defaults thinking.display to "omitted"; we must request
    // "summarized" to keep reasoning blocks populated.
    auto cfg = build_thinking_config("claude-opus-4-7", "xhigh", 4096);
    ASSERT_TRUE(cfg.contains("thinking"));
    EXPECT_EQ(cfg["thinking"].value("type", ""), "adaptive");
    EXPECT_EQ(cfg["thinking"].value("display", ""), "summarized");
    ASSERT_TRUE(cfg.contains("output_config"));
    // xhigh on 4.7+ is a distinct level (not collapsed to max).
    EXPECT_EQ(cfg["output_config"].value("effort", ""), "xhigh");
}

TEST(AnthropicFeatures, BuildThinking46RequestsSummarizedDisplay) {
    // 4.6 also uses adaptive thinking and should also request summarized,
    // since that was the pre-4.7 default behaviour we preserve.
    auto cfg = build_thinking_config("claude-opus-4-6", "xhigh", 4096);
    ASSERT_TRUE(cfg.contains("thinking"));
    EXPECT_EQ(cfg["thinking"].value("display", ""), "summarized");
    // xhigh on pre-4.7 must collapse to max to avoid a 400.
    EXPECT_EQ(cfg["output_config"].value("effort", ""), "max");
}

TEST(AnthropicFeatures, StopReasonContextWindowExceededIsLength) {
    // Claude 4.5+/4.7 added model_context_window_exceeded — must map
    // to OpenAI finish_reason "length" so downstream compression kicks in.
    EXPECT_EQ(map_anthropic_stop_reason("model_context_window_exceeded"), "length");
}

TEST(AnthropicFeatures, HaikuRejectedByExtendedThinking) {
    EXPECT_FALSE(supports_extended_thinking("claude-haiku-4-5"));
    EXPECT_FALSE(supports_extended_thinking("claude-3-5-haiku"));
    EXPECT_TRUE(supports_extended_thinking("claude-sonnet-4-6"));
    EXPECT_TRUE(supports_extended_thinking("claude-opus-4"));
}

TEST(AnthropicFeatures, BuildThinkingAdaptive) {
    auto cfg = build_thinking_config("claude-opus-4-6", "high", 4096);
    ASSERT_TRUE(cfg.contains("thinking"));
    EXPECT_EQ(cfg["thinking"].value("type", ""), "adaptive");
    ASSERT_TRUE(cfg.contains("output_config"));
    EXPECT_EQ(cfg["output_config"].value("effort", ""), "high");
    // Adaptive does NOT override temperature/max_tokens.
    EXPECT_FALSE(cfg.contains("temperature"));
    EXPECT_FALSE(cfg.contains("max_tokens"));
}

TEST(AnthropicFeatures, BuildThinkingManualBudget) {
    auto cfg = build_thinking_config("claude-opus-4", "medium", 4096);
    ASSERT_TRUE(cfg.contains("thinking"));
    EXPECT_EQ(cfg["thinking"].value("type", ""), "enabled");
    EXPECT_EQ(cfg["thinking"].value("budget_tokens", 0), 8000);
    EXPECT_EQ(cfg.value("temperature", 0), 1);
    // max_tokens is bumped to budget + 4096.
    EXPECT_EQ(cfg.value("max_tokens", 0), 12096);
}

TEST(AnthropicFeatures, BuildThinkingHaikuReturnsEmpty) {
    auto cfg = build_thinking_config("claude-haiku-4-5", "high", 4096);
    EXPECT_TRUE(cfg.empty());
}

TEST(AnthropicFeatures, StopReasonMapping) {
    EXPECT_EQ(map_anthropic_stop_reason("end_turn"),     "stop");
    EXPECT_EQ(map_anthropic_stop_reason("tool_use"),     "tool_calls");
    EXPECT_EQ(map_anthropic_stop_reason("max_tokens"),   "length");
    EXPECT_EQ(map_anthropic_stop_reason("stop_sequence"), "stop");
    EXPECT_EQ(map_anthropic_stop_reason("refusal"),      "content_filter");
    EXPECT_EQ(map_anthropic_stop_reason(""),             "stop");
    EXPECT_EQ(map_anthropic_stop_reason("unknown"),      "stop");
}

TEST(AnthropicFeatures, ParseExtrasAllFields) {
    nlohmann::json extra = {
        {"stop_sequences", {"###", "END"}},
        {"top_p", 0.9},
        {"top_k", 40},
        {"service_tier", "priority"},
        {"tool_choice", "auto"},
        {"thinking_effort", "high"},
        {"fast_mode", true},
        {"is_oauth", true},
    };
    auto x = parse_anthropic_extras(extra);
    ASSERT_EQ(x.stop_sequences.size(), 2u);
    EXPECT_EQ(x.stop_sequences[0], "###");
    ASSERT_TRUE(x.top_p.has_value()); EXPECT_NEAR(*x.top_p, 0.9, 1e-9);
    ASSERT_TRUE(x.top_k.has_value()); EXPECT_EQ(*x.top_k, 40);
    EXPECT_EQ(*x.service_tier, "priority");
    EXPECT_EQ(*x.tool_choice, "auto");
    EXPECT_EQ(*x.thinking_effort, "high");
    EXPECT_TRUE(x.fast_mode);
    EXPECT_TRUE(x.is_oauth);
}

TEST(AnthropicFeatures, ParseExtrasEmpty) {
    auto x = parse_anthropic_extras(nlohmann::json::object());
    EXPECT_TRUE(x.stop_sequences.empty());
    EXPECT_FALSE(x.top_p.has_value());
    EXPECT_FALSE(x.is_oauth);
}

TEST(AnthropicFeatures, ThirdPartyEndpointDetection) {
    EXPECT_FALSE(is_third_party_anthropic_endpoint("https://api.anthropic.com/v1"));
    EXPECT_TRUE(is_third_party_anthropic_endpoint("https://api.z.ai/v1"));
    EXPECT_TRUE(is_third_party_anthropic_endpoint("https://openrouter.ai/api/v1"));
    EXPECT_TRUE(is_third_party_anthropic_endpoint("https://api.moonshot.cn/v1"));
    EXPECT_FALSE(is_third_party_anthropic_endpoint(""));
}

TEST(AnthropicFeatures, CommonBetasForNativeEndpoint) {
    auto betas = common_betas_for_base_url("https://api.anthropic.com/v1");
    EXPECT_FALSE(betas.empty());
    bool has_streaming = false, has_thinking = false;
    for (const auto& b : betas) {
        if (b.find("fine-grained-tool-streaming") != std::string::npos)
            has_streaming = true;
        if (b.find("interleaved-thinking") != std::string::npos)
            has_thinking = true;
    }
    EXPECT_TRUE(has_streaming);
    EXPECT_TRUE(has_thinking);
}

TEST(AnthropicFeatures, CommonBetasForThirdParty) {
    auto betas = common_betas_for_base_url("https://openrouter.ai/api/v1");
    // Third-party endpoints get the trimmed list — NO fine-grained-tool-streaming.
    for (const auto& b : betas) {
        EXPECT_EQ(b.find("fine-grained-tool-streaming"), std::string::npos);
    }
}

TEST(AnthropicFeatures, ClaudeCodeSanitization) {
    EXPECT_EQ(sanitize_for_claude_code_oauth("Hermes Agent is great"),
              "Claude Code is great");
    EXPECT_EQ(sanitize_for_claude_code_oauth("product hermes-agent"),
              "product claude-code");
    EXPECT_EQ(sanitize_for_claude_code_oauth("by Nous Research"),
              "by Anthropic");
    EXPECT_EQ(sanitize_for_claude_code_oauth("nothing here"),
              "nothing here");
}

TEST(AnthropicFeatures, McpToolPrefix) {
    EXPECT_EQ(apply_mcp_tool_prefix("search_code"), "mcp_search_code");
    EXPECT_EQ(apply_mcp_tool_prefix("mcp_search"), "mcp_search");
    EXPECT_EQ(strip_mcp_tool_prefix("mcp_read_file"), "read_file");
    EXPECT_EQ(strip_mcp_tool_prefix("read_file"), "read_file");
}

TEST(AnthropicFeatures, NormalizeStopSequences) {
    auto r = normalize_stop_sequences({"###", "", "###", "END", "STOP", "!", "extra"});
    ASSERT_EQ(r.size(), 4u);
    EXPECT_EQ(r[0], "###");
    EXPECT_EQ(r[1], "END");
    EXPECT_EQ(r[2], "STOP");
    EXPECT_EQ(r[3], "!");
}

TEST(AnthropicFeatures, ExtractReasoningBlocks) {
    nlohmann::json content = nlohmann::json::array();
    content.push_back({{"type", "thinking"},
                       {"thinking", "First step"},
                       {"signature", "sig1"}});
    content.push_back({{"type", "text"}, {"text", "hello"}});
    content.push_back({{"type", "thinking"},
                       {"thinking", "Second step"}});
    content.push_back({{"type", "redacted_thinking"},
                       {"data", "abc"},
                       {"signature", "sig2"}});

    auto e = extract_reasoning_blocks(content);
    EXPECT_EQ(e.text, "First step\n\nSecond step");
    EXPECT_EQ(e.blocks.size(), 3u);
    EXPECT_TRUE(e.has_signature);
}

TEST(AnthropicFeatures, ExtractReasoningBlocksEmpty) {
    auto e = extract_reasoning_blocks(nlohmann::json::array());
    EXPECT_TRUE(e.text.empty());
    EXPECT_TRUE(e.blocks.empty());
    EXPECT_FALSE(e.has_signature);
}

TEST(AnthropicFeatures, InspectCacheBreakpoints) {
    std::vector<Message> msgs;
    Message sys;
    sys.role = Role::System;
    sys.content_text = "sys";
    msgs.push_back(sys);
    for (int i = 0; i < 6; ++i) {
        Message u;
        u.role = (i % 2 == 0) ? Role::User : Role::Assistant;
        u.content_text = "turn " + std::to_string(i);
        msgs.push_back(u);
    }

    PromptCacheOptions opts;
    opts.native_anthropic = true;
    apply_anthropic_cache_control(msgs, opts);

    auto info = inspect_cache_breakpoints(msgs);
    EXPECT_EQ(info.total_breakpoints, 4);
    EXPECT_EQ(info.system_breakpoints, 1);
    EXPECT_EQ(info.message_breakpoints, 3);
}

TEST(AnthropicFeatures, NormalizeStopSequencesDedupe) {
    auto r = normalize_stop_sequences({"a", "a", "b", "b", "c"});
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0], "a");
    EXPECT_EQ(r[1], "b");
    EXPECT_EQ(r[2], "c");
}
