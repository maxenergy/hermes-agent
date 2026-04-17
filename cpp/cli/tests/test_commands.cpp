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

// ---------------------------------------------------------------------
// Newly-ported commands: /clear /history /save /config /statusbar /skin
// /toolsets /cron /browser /plugins /paste /image.  Each must resolve
// through `resolve_command` so they can be dispatched and appear in
// `/help` output.
// ---------------------------------------------------------------------

TEST(Commands, PortedSessionCommandsResolve) {
    for (const char* name : {"clear", "history", "save"}) {
        auto cmd = resolve_command(name);
        ASSERT_TRUE(cmd.has_value()) << name;
        EXPECT_EQ(cmd->category, "Session") << name;
        EXPECT_TRUE(cmd->cli_only) << name;
    }
}

TEST(Commands, PortedConfigCommandsResolve) {
    auto c = resolve_command("config");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->category, "Configuration");
    EXPECT_TRUE(c->cli_only);

    auto sb = resolve_command("statusbar");
    ASSERT_TRUE(sb.has_value());
    // Alias "sb" must resolve to the same canonical command.
    auto sb_alias = resolve_command("sb");
    ASSERT_TRUE(sb_alias.has_value());
    EXPECT_EQ(sb_alias->name, "statusbar");

    auto skin = resolve_command("skin");
    ASSERT_TRUE(skin.has_value());
    EXPECT_TRUE(skin->cli_only);
}

TEST(Commands, PortedToolsSkillsCommandsResolve) {
    for (const char* name : {"toolsets", "cron", "browser", "plugins"}) {
        auto cmd = resolve_command(name);
        ASSERT_TRUE(cmd.has_value()) << name;
        EXPECT_EQ(cmd->category, "Tools & Skills") << name;
        EXPECT_TRUE(cmd->cli_only) << name;
    }
}

TEST(Commands, PortedInfoCommandsResolve) {
    for (const char* name : {"paste", "image"}) {
        auto cmd = resolve_command(name);
        ASSERT_TRUE(cmd.has_value()) << name;
        EXPECT_EQ(cmd->category, "Info") << name;
        EXPECT_TRUE(cmd->cli_only) << name;
    }
    // /image expects an argument hint.
    auto image = resolve_command("image");
    ASSERT_TRUE(image.has_value());
    EXPECT_NE(image->args_hint.find("<path>"), std::string::npos);
}

TEST(Commands, PlatformsHasGatewayAlias) {
    // Python: CommandDef("platforms", ..., aliases=("gateway",)).
    auto cmd = resolve_command("gateway");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->name, "platforms");
}

TEST(Commands, CliOnlyCommandsExcludedFromGateway) {
    const auto& gw = gateway_known_commands();
    // Newly-added cli_only entries must not leak to the gateway known set.
    for (const char* name : {"clear", "history", "save", "config",
                             "statusbar", "skin", "toolsets", "cron",
                             "browser", "plugins", "paste", "image"}) {
        EXPECT_EQ(gw.count(name), 0u) << "cli_only leaked: " << name;
    }
}
