// Tests for the Honcho memory provider.
#include <gtest/gtest.h>

#include "hermes/agent/honcho_provider.hpp"
#include "hermes/llm/llm_client.hpp"

using namespace hermes::agent;
using namespace hermes::llm;

namespace {

HonchoMemoryProvider::Config make_cfg() {
    HonchoMemoryProvider::Config c;
    c.api_url = "https://api.honcho.dev/v1";
    c.api_key = "test-key";
    c.app_id = "test-app";
    c.user_id = "user-42";
    return c;
}

}  // namespace

TEST(HonchoProvider, ConfigValidationDetectsMissingFields) {
    HonchoMemoryProvider::Config c;
    FakeHttpTransport fake;
    HonchoMemoryProvider p(c, &fake);
    EXPECT_FALSE(p.config_valid());

    c.api_key = "x";
    c.app_id = "a";
    c.user_id = "u";
    HonchoMemoryProvider p2(c, &fake);
    EXPECT_TRUE(p2.config_valid());
}

TEST(HonchoProvider, SyncPostsUserAndAssistantMessages) {
    FakeHttpTransport fake;
    // First response: session creation.
    fake.enqueue_response({200, R"({"id":"sess-1"})", {}});
    // Then two message posts.
    fake.enqueue_response({200, R"({"ok":true})", {}});
    fake.enqueue_response({200, R"({"ok":true})", {}});

    HonchoMemoryProvider p(make_cfg(), &fake);
    p.sync("hello", "hi there");
    ASSERT_GE(fake.requests().size(), 3u);
    EXPECT_NE(fake.requests()[0].url.find("/sessions"), std::string::npos);
    EXPECT_EQ(p.session_id(), "sess-1");
    EXPECT_NE(fake.requests()[1].url.find("/messages"), std::string::npos);
    EXPECT_NE(fake.requests()[1].body.find("hello"), std::string::npos);
    EXPECT_NE(fake.requests()[2].body.find("hi there"), std::string::npos);
}

TEST(HonchoProvider, PrefetchFetchesInsights) {
    FakeHttpTransport fake;
    fake.enqueue_response(
        {200, R"({"insights":["user loves haskell","prefers vim"]})", {}});

    HonchoMemoryProvider p(make_cfg(), &fake);
    p.prefetch("whatever");
    ASSERT_EQ(p.cached_insights().size(), 2u);
    EXPECT_EQ(p.cached_insights()[0], "user loves haskell");
    EXPECT_EQ(p.cached_insights()[1], "prefers vim");
}

TEST(HonchoProvider, BuildSystemPromptSectionEmitsUserContextHeader) {
    FakeHttpTransport fake;
    HonchoMemoryProvider p(make_cfg(), &fake);
    p.set_cached_insights_for_test({"first", "second"});
    auto section = p.build_system_prompt_section();
    EXPECT_NE(section.find("## User Context"), std::string::npos);
    EXPECT_NE(section.find("first"), std::string::npos);
    EXPECT_NE(section.find("second"), std::string::npos);
}

TEST(HonchoProvider, EmptyCacheProducesEmptySection) {
    FakeHttpTransport fake;
    HonchoMemoryProvider p(make_cfg(), &fake);
    EXPECT_TRUE(p.build_system_prompt_section().empty());
}

TEST(HonchoProvider, NameAndExternalFlags) {
    FakeHttpTransport fake;
    HonchoMemoryProvider p(make_cfg(), &fake);
    EXPECT_EQ(p.name(), "honcho");
    EXPECT_TRUE(p.is_external());
}
