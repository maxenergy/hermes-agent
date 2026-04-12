#include "hermes/cli/commands.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace hermes::cli;

TEST(Commands, ResolveNew) {
    auto cmd = resolve_command("new");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "new");
    EXPECT_EQ(cmd->category, "Session");
}

TEST(Commands, ResolveAliasBg) {
    auto cmd = resolve_command("bg");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "background");
}

TEST(Commands, UnknownReturnsNullopt) {
    auto cmd = resolve_command("xyzzy_nonexistent");
    EXPECT_FALSE(cmd.has_value());
}

TEST(Commands, CommandsByCategoryHasAllCategories) {
    auto by_cat = commands_by_category();
    std::set<std::string> expected = {
        "Session", "Configuration", "Tools & Skills", "Info", "Exit", "Gateway"};
    for (const auto& cat : expected) {
        EXPECT_TRUE(by_cat.count(cat) > 0) << "Missing category: " << cat;
    }
}

TEST(Commands, GatewayHelpLinesNonEmpty) {
    auto lines = gateway_help_lines();
    EXPECT_FALSE(lines.empty());
    // Should contain /help at minimum.
    bool found_help = false;
    for (const auto& l : lines) {
        if (l.find("/help") != std::string::npos) found_help = true;
    }
    EXPECT_TRUE(found_help);
}

TEST(Commands, TelegramBotCommandsExcludesCliOnly) {
    auto tg = telegram_bot_commands();
    for (const auto& [name, desc] : tg) {
        // "new" is cli_only, so it must not appear.
        EXPECT_NE(name, "new") << "cli_only command leaked to Telegram";
    }
}

TEST(Commands, RegistryHasAtLeast30Commands) {
    EXPECT_GE(command_registry().size(), 30u);
}

TEST(Commands, SlackSubcommandMapContainsModel) {
    auto m = slack_subcommand_map();
    EXPECT_TRUE(m.count("model") > 0);
}

TEST(Commands, ResolveWithLeadingSlash) {
    auto cmd = resolve_command("/help");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "help");
}

TEST(Commands, CommandsFlatIncludesAliases) {
    auto flat = commands_flat();
    EXPECT_TRUE(flat.count("bg") > 0);
    EXPECT_TRUE(flat.count("background") > 0);
    EXPECT_EQ(flat["bg"].name, "background");
}
