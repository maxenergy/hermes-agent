#include "hermes/llm/prompt_cache.hpp"
#include "hermes/llm/message.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using hermes::llm::ContentBlock;
using hermes::llm::Message;
using hermes::llm::PromptCacheOptions;
using hermes::llm::Role;

namespace {

Message text_msg(Role r, const std::string& text) {
    Message m;
    m.role = r;
    ContentBlock b;
    b.type = "text";
    b.text = text;
    m.content_blocks.push_back(b);
    return m;
}

int count_cache_markers(const std::vector<Message>& msgs) {
    int count = 0;
    for (const auto& m : msgs) {
        if (m.cache_control) ++count;
        for (const auto& b : m.content_blocks) {
            if (b.cache_control) ++count;
        }
    }
    return count;
}

}  // namespace

TEST(PromptCache, NoOpWhenNotNativeAnthropic) {
    std::vector<Message> msgs = {
        text_msg(Role::System, "sys"),
        text_msg(Role::User, "u1"),
        text_msg(Role::Assistant, "a1"),
    };
    PromptCacheOptions opts;
    opts.native_anthropic = false;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);
    EXPECT_EQ(0, count_cache_markers(msgs));
}

TEST(PromptCache, SystemPlusFiveTurns_FourMarkers) {
    std::vector<Message> msgs = {
        text_msg(Role::System, "sys"),
        text_msg(Role::User, "u1"),
        text_msg(Role::Assistant, "a1"),
        text_msg(Role::User, "u2"),
        text_msg(Role::Assistant, "a2"),
        text_msg(Role::User, "u3"),
    };
    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);

    EXPECT_EQ(4, count_cache_markers(msgs));
    // System + last 3 (a2 is second-to-last when u3 is last — actually
    // last 3 are msgs[3..5]).
    ASSERT_TRUE(msgs[0].content_blocks[0].cache_control.has_value());  // sys
    ASSERT_TRUE(msgs[3].content_blocks[0].cache_control.has_value());  // u2
    ASSERT_TRUE(msgs[4].content_blocks[0].cache_control.has_value());  // a2
    ASSERT_TRUE(msgs[5].content_blocks[0].cache_control.has_value());  // u3
    EXPECT_FALSE(msgs[1].content_blocks[0].cache_control.has_value());
    EXPECT_FALSE(msgs[2].content_blocks[0].cache_control.has_value());
}

TEST(PromptCache, NoSystem_OnlyLastThree) {
    std::vector<Message> msgs = {
        text_msg(Role::User, "u1"),
        text_msg(Role::Assistant, "a1"),
        text_msg(Role::User, "u2"),
        text_msg(Role::Assistant, "a2"),
        text_msg(Role::User, "u3"),
    };
    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);
    // No system → 4 breakpoints available for non-system tail, but only
    // 3 messages qualify (last 3) per the spec.  Actually the Python
    // impl takes non_sys[-(4-0):] = last 4 when no system.  Our port
    // mirrors that — let's verify count is 4.
    EXPECT_EQ(4, count_cache_markers(msgs));
}

TEST(PromptCache, Idempotent) {
    std::vector<Message> msgs = {
        text_msg(Role::System, "sys"),
        text_msg(Role::User, "u1"),
        text_msg(Role::Assistant, "a1"),
        text_msg(Role::User, "u2"),
    };
    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);
    const int first_count = count_cache_markers(msgs);
    hermes::llm::apply_anthropic_cache_control(msgs, opts);
    const int second_count = count_cache_markers(msgs);
    EXPECT_EQ(first_count, second_count);
    // Hard invariant: <= 4 markers total after any number of applies.
    EXPECT_LE(second_count, 4);
}

TEST(PromptCache, MarkerLandsOnLastContentBlock) {
    std::vector<Message> msgs;
    Message m;
    m.role = Role::User;
    ContentBlock b1;
    b1.type = "text";
    b1.text = "first part";
    ContentBlock b2;
    b2.type = "text";
    b2.text = "second part — this is where the marker lives";
    m.content_blocks.push_back(b1);
    m.content_blocks.push_back(b2);
    msgs.push_back(std::move(m));

    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);

    EXPECT_FALSE(msgs[0].content_blocks[0].cache_control.has_value());
    ASSERT_TRUE(msgs[0].content_blocks[1].cache_control.has_value());
}

TEST(PromptCache, PromotesTextContentToBlock) {
    std::vector<Message> msgs;
    Message m;
    m.role = Role::System;
    m.content_text = "you are helpful";
    msgs.push_back(m);

    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);

    ASSERT_EQ(msgs[0].content_blocks.size(), 1u);
    ASSERT_TRUE(msgs[0].content_blocks[0].cache_control.has_value());
}

TEST(PromptCache, EmptyInput) {
    std::vector<Message> msgs;
    PromptCacheOptions opts;
    opts.native_anthropic = true;
    hermes::llm::apply_anthropic_cache_control(msgs, opts);
    EXPECT_TRUE(msgs.empty());
}
