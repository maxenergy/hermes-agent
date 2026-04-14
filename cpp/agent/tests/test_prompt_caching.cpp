// Tests for hermes::agent::apply_anthropic_cache_control.
#include "hermes/agent/prompt_caching.hpp"

#include <gtest/gtest.h>

using hermes::agent::apply_anthropic_cache_control;

namespace {

nlohmann::json make_msg(const std::string& role, const std::string& text) {
    return {{"role", role}, {"content", text}};
}

bool has_cache_control(const nlohmann::json& msg) {
    if (msg.contains("cache_control")) return true;
    auto it = msg.find("content");
    if (it == msg.end()) return false;
    if (it->is_array()) {
        for (const auto& part : *it) {
            if (part.is_object() && part.contains("cache_control")) return true;
        }
    }
    return false;
}

}  // namespace

TEST(PromptCaching, EmptyMessagesPassthrough) {
    nlohmann::json msgs = nlohmann::json::array();
    auto out = apply_anthropic_cache_control(msgs);
    EXPECT_TRUE(out.is_array());
    EXPECT_TRUE(out.empty());
}

TEST(PromptCaching, SystemAndThreeBreakpoints) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("system", "sys"),
        make_msg("user", "u1"),
        make_msg("assistant", "a1"),
        make_msg("user", "u2"),
        make_msg("assistant", "a2"),
        make_msg("user", "u3"),
    });
    auto out = apply_anthropic_cache_control(msgs);
    EXPECT_TRUE(has_cache_control(out[0]));   // system
    // Last three non-system messages: indices 3, 4, 5.
    EXPECT_FALSE(has_cache_control(out[1]));
    EXPECT_FALSE(has_cache_control(out[2]));
    EXPECT_TRUE(has_cache_control(out[3]));
    EXPECT_TRUE(has_cache_control(out[4]));
    EXPECT_TRUE(has_cache_control(out[5]));
}

TEST(PromptCaching, NoSystemStillCachesLastThree) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("user", "u1"),
        make_msg("assistant", "a1"),
        make_msg("user", "u2"),
        make_msg("assistant", "a2"),
        make_msg("user", "u3"),
    });
    auto out = apply_anthropic_cache_control(msgs);
    // 4 breakpoints available: last 4 messages cached.
    EXPECT_FALSE(has_cache_control(out[0]));
    EXPECT_TRUE(has_cache_control(out[1]));
    EXPECT_TRUE(has_cache_control(out[2]));
    EXPECT_TRUE(has_cache_control(out[3]));
    EXPECT_TRUE(has_cache_control(out[4]));
}

TEST(PromptCaching, StringContentConvertedToArray) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("system", "sys"),
        make_msg("user", "hi"),
    });
    auto out = apply_anthropic_cache_control(msgs);
    EXPECT_TRUE(out[0]["content"].is_array());
    EXPECT_EQ(out[0]["content"][0]["type"], "text");
    EXPECT_EQ(out[0]["content"][0]["text"], "sys");
    EXPECT_TRUE(out[0]["content"][0].contains("cache_control"));
}

TEST(PromptCaching, OneHourTtlMarker) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("system", "sys"),
        make_msg("user", "hi"),
    });
    auto out = apply_anthropic_cache_control(msgs, "1h");
    // Locate the cache_control on the system message.
    ASSERT_TRUE(out[0]["content"].is_array());
    const auto& cc = out[0]["content"][0]["cache_control"];
    EXPECT_EQ(cc.value("ttl", std::string()), "1h");
    EXPECT_EQ(cc.value("type", std::string()), "ephemeral");
}

TEST(PromptCaching, ToolMessageOnlyCachedWhenNative) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("system", "sys"),
        make_msg("user", "u"),
        {{"role", "tool"}, {"content", "out"}, {"tool_call_id", "x"}},
    });
    auto non_native = apply_anthropic_cache_control(msgs, "5m", /*native=*/false);
    EXPECT_FALSE(non_native[2].contains("cache_control"));
    auto native = apply_anthropic_cache_control(msgs, "5m", /*native=*/true);
    EXPECT_TRUE(native[2].contains("cache_control"));
}

TEST(PromptCaching, ArrayContentMarksLastPart) {
    nlohmann::json msgs = nlohmann::json::array({
        make_msg("system", "sys"),
        {{"role", "user"}, {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "a"}},
            {{"type", "text"}, {"text", "b"}},
        })}},
    });
    auto out = apply_anthropic_cache_control(msgs);
    EXPECT_FALSE(out[1]["content"][0].contains("cache_control"));
    EXPECT_TRUE(out[1]["content"][1].contains("cache_control"));
}
