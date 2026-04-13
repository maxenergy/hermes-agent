// Tests for Slack /hermes subcommand routing.
#include <gtest/gtest.h>

#include <hermes/gateway/slack_subcommand_router.hpp>

using hermes::gateway::parse_slack_subcommand;
using hermes::gateway::SlackSubcommandRouter;

TEST(ParseSlackSubcommand, EmptyReturnsNullopt) {
    EXPECT_FALSE(parse_slack_subcommand("").has_value());
    EXPECT_FALSE(parse_slack_subcommand("   ").has_value());
}

TEST(ParseSlackSubcommand, BareCommand) {
    auto r = parse_slack_subcommand("status");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "status");
    EXPECT_EQ(r->remainder, "");
    EXPECT_TRUE(r->argv.empty());
}

TEST(ParseSlackSubcommand, StripLeadingSlash) {
    auto r = parse_slack_subcommand("/new");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "new");
}

TEST(ParseSlackSubcommand, ArgvSplit) {
    auto r = parse_slack_subcommand("background  hello world  ");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "background");
    EXPECT_EQ(r->remainder, "hello world");
    ASSERT_EQ(r->argv.size(), 2u);
    EXPECT_EQ(r->argv[0], "hello");
    EXPECT_EQ(r->argv[1], "world");
}

TEST(ParseSlackSubcommand, NameLowercased) {
    auto r = parse_slack_subcommand("STATUS");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->name, "status");
}

TEST(SlackSubcommandRouter, DispatchesRegisteredHandler) {
    SlackSubcommandRouter router;
    router.register_handler("ping", [](const auto& sc) {
        return "pong:" + sc.remainder;
    });
    EXPECT_EQ(router.dispatch("ping hello"), "pong:hello");
}

TEST(SlackSubcommandRouter, UnknownReportsKnownList) {
    SlackSubcommandRouter router;
    router.set_known_commands({{"new", "Start new"}, {"status", "Show"}});
    auto out = router.dispatch("nonexistent foo");
    EXPECT_NE(out.find("unknown command"), std::string::npos);
    EXPECT_NE(out.find("new"), std::string::npos);
    EXPECT_NE(out.find("status"), std::string::npos);
}

TEST(SlackSubcommandRouter, EmptyBodyReturnsHelp) {
    SlackSubcommandRouter router;
    router.set_known_commands({{"help", "Help"}});
    auto out = router.dispatch("");
    EXPECT_NE(out.find("Usage"), std::string::npos);
    EXPECT_NE(out.find("help"), std::string::npos);
}

TEST(SlackSubcommandRouter, HasHandlerCaseInsensitive) {
    SlackSubcommandRouter router;
    router.register_handler("New", [](const auto&) { return ""; });
    EXPECT_TRUE(router.has_handler("new"));
    EXPECT_TRUE(router.has_handler("NEW"));
}
