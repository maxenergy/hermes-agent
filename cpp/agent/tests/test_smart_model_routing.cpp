// Tests for hermes::agent::choose_cheap_model_route.
#include "hermes/agent/smart_model_routing.hpp"

#include <gtest/gtest.h>

using hermes::agent::choose_cheap_model_route;

namespace {

nlohmann::json make_config(bool enabled = true) {
    return nlohmann::json{
        {"enabled", enabled},
        {"max_simple_chars", 160},
        {"max_simple_words", 28},
        {"cheap_model", {{"provider", "openrouter"}, {"model", "gpt-4o-mini"}}},
    };
}

}  // namespace

TEST(SmartModelRouting, DisabledReturnsNullopt) {
    auto r = choose_cheap_model_route("hello", make_config(false));
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, MissingCheapModelReturnsNullopt) {
    nlohmann::json cfg{{"enabled", true}};
    EXPECT_FALSE(choose_cheap_model_route("hi", cfg).has_value());
}

TEST(SmartModelRouting, SimpleGreetingRoutes) {
    auto r = choose_cheap_model_route("hi there!", make_config());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->provider, "openrouter");
    EXPECT_EQ(r->model, "gpt-4o-mini");
    EXPECT_EQ(r->routing_reason, "simple_turn");
}

TEST(SmartModelRouting, CodeFenceForcesPrimary) {
    auto r = choose_cheap_model_route("hello ```code```", make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, InlineBacktickForcesPrimary) {
    auto r = choose_cheap_model_route("please run `ls`", make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, UrlForcesPrimary) {
    auto r = choose_cheap_model_route("see https://example.com", make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, MultiLineForcesPrimary) {
    auto r = choose_cheap_model_route("first line\nsecond line\nthird", make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, ComplexKeywordForcesPrimary) {
    EXPECT_FALSE(choose_cheap_model_route("please debug this", make_config()).has_value());
    EXPECT_FALSE(choose_cheap_model_route("refactor my function", make_config()).has_value());
    EXPECT_FALSE(choose_cheap_model_route("plan the release", make_config()).has_value());
    EXPECT_FALSE(choose_cheap_model_route("run pytest", make_config()).has_value());
}

TEST(SmartModelRouting, LongMessageForcesPrimary) {
    std::string msg(200, 'x');
    auto r = choose_cheap_model_route(msg, make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, TooManyWordsForcesPrimary) {
    std::string msg;
    for (int i = 0; i < 30; ++i) msg += "word ";
    auto r = choose_cheap_model_route(msg, make_config());
    EXPECT_FALSE(r.has_value());
}

TEST(SmartModelRouting, EmptyMessageReturnsNullopt) {
    EXPECT_FALSE(choose_cheap_model_route("", make_config()).has_value());
    EXPECT_FALSE(choose_cheap_model_route("   \t\n  ", make_config()).has_value());
}

TEST(SmartModelRouting, PreservesExtrasAndLowersProvider) {
    nlohmann::json cfg = make_config();
    cfg["cheap_model"]["provider"] = "OpenRouter";
    cfg["cheap_model"]["api_key_env"] = "FOO_KEY";
    auto r = choose_cheap_model_route("hello friend", cfg);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->provider, "openrouter");
    EXPECT_EQ(r->api_key_env, "FOO_KEY");
    EXPECT_EQ(r->extras["provider"], "OpenRouter");  // original preserved
}
