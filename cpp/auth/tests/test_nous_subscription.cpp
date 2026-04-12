#include <gtest/gtest.h>

#include "hermes/auth/nous_subscription.hpp"
#include "hermes/llm/llm_client.hpp"

using hermes::auth::check_subscription;
using hermes::auth::clear_subscription_cache;
using hermes::llm::FakeHttpTransport;

TEST(NousSubscription, ParsesActiveProSubscription) {
    FakeHttpTransport fake;
    fake.enqueue_response({200,
                           R"({"active":true,"tier":"pro",)"
                           R"("expires_at":"2026-12-31T23:59:59Z",)"
                           R"("features":["vision","voice"],"user_id":"u_42"})",
                           {}});
    auto sub = check_subscription("test-key", &fake, "https://api.test");
    ASSERT_TRUE(sub.has_value());
    EXPECT_TRUE(sub->active);
    EXPECT_EQ(sub->tier, "pro");
    EXPECT_EQ(sub->user_id, "u_42");
    EXPECT_EQ(sub->features.size(), 2u);
}

TEST(NousSubscription, Returns401AsNullopt) {
    FakeHttpTransport fake;
    fake.enqueue_response({401, R"({"error":"unauthorized"})", {}});
    auto sub = check_subscription("bad-key", &fake, "https://api.test");
    EXPECT_FALSE(sub.has_value());
}

TEST(NousSubscription, EmptyApiKeyReturnsNullopt) {
    FakeHttpTransport fake;
    auto sub = check_subscription("", &fake);
    EXPECT_FALSE(sub.has_value());
}

TEST(NousSubscription, MalformedJsonReturnsNullopt) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "not json", {}});
    auto sub = check_subscription("test-key", &fake, "https://api.test");
    EXPECT_FALSE(sub.has_value());
}

TEST(NousSubscription, CacheClearDoesNotCrash) {
    clear_subscription_cache();
    clear_subscription_cache();  // idempotent
    SUCCEED();
}
