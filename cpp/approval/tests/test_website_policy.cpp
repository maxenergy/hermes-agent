#include "hermes/approval/website_policy.hpp"

#include <gtest/gtest.h>

using hermes::approval::DomainRule;
using hermes::approval::WebsitePolicy;

TEST(WebsitePolicy, AllowRule) {
    WebsitePolicy policy;
    policy.add_rule({"example.com", true});
    EXPECT_TRUE(policy.is_allowed("https://example.com/page"));
}

TEST(WebsitePolicy, BlockRule) {
    WebsitePolicy policy;
    policy.add_rule({"blocked.com", false});
    EXPECT_FALSE(policy.is_allowed("https://blocked.com/page"));
}

TEST(WebsitePolicy, WildcardMatchesSubdomain) {
    WebsitePolicy policy;
    policy.add_rule({"*.example.com", false});
    EXPECT_FALSE(policy.is_allowed("https://sub.example.com/page"));
    EXPECT_FALSE(policy.is_allowed("https://example.com/page"));
}

TEST(WebsitePolicy, WildcardDoesNotMatchOtherDomain) {
    WebsitePolicy policy;
    policy.add_rule({"*.example.com", false});
    EXPECT_TRUE(policy.is_allowed("https://other.com/page"));
}

TEST(WebsitePolicy, DefaultIsAllow) {
    WebsitePolicy policy;
    // No rules — everything is allowed by default.
    EXPECT_TRUE(policy.is_allowed("https://anything.com"));
}

TEST(WebsitePolicy, FirstMatchWins) {
    WebsitePolicy policy;
    policy.add_rule({"example.com", false});  // block
    policy.add_rule({"example.com", true});   // allow (won't fire)
    EXPECT_FALSE(policy.is_allowed("https://example.com/page"));
}
