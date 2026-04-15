#include "hermes/cli/commands_depth.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::commands_depth;

namespace {
CommandSpec mk(const std::string& name, const std::string& desc,
               const std::string& args = "",
               std::initializer_list<std::string> aliases = {},
               bool cli_only = false, bool gateway_only = false) {
    CommandSpec c;
    c.name = name;
    c.description = desc;
    c.args_hint = args;
    for (const auto& a : aliases) c.aliases.push_back(a);
    c.cli_only = cli_only;
    c.gateway_only = gateway_only;
    return c;
}
}  // namespace

TEST(CommandsDepth, SanitizeTelegramBasic) {
    EXPECT_EQ(sanitize_telegram_name("new"), "new");
    EXPECT_EQ(sanitize_telegram_name("NEW"), "new");
}

TEST(CommandsDepth, SanitizeTelegramHyphenToUnderscore) {
    EXPECT_EQ(sanitize_telegram_name("set-home"), "set_home");
}

TEST(CommandsDepth, SanitizeTelegramStripsInvalidChars) {
    EXPECT_EQ(sanitize_telegram_name("my!bad#name"), "mybadname");
    EXPECT_EQ(sanitize_telegram_name("skill/path"), "skillpath");
}

TEST(CommandsDepth, SanitizeTelegramCollapsesUnderscores) {
    EXPECT_EQ(sanitize_telegram_name("a__b___c"), "a_b_c");
}

TEST(CommandsDepth, SanitizeTelegramStripsEdgeUnderscores) {
    EXPECT_EQ(sanitize_telegram_name("__hello__"), "hello");
}

TEST(CommandsDepth, SanitizeTelegramAllInvalidYieldsEmpty) {
    EXPECT_EQ(sanitize_telegram_name("!!!"), "");
    EXPECT_EQ(sanitize_telegram_name(""), "");
}

TEST(CommandsDepth, ClampNoTruncationPreserved) {
    auto out = clamp_command_names(
        {{"foo", "desc a"}, {"bar", "desc b"}},
        /*reserved=*/{});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "foo");
    EXPECT_EQ(out[1].first, "bar");
}

TEST(CommandsDepth, ClampTruncatesAt32) {
    std::string long_name(40, 'a');
    auto out = clamp_command_names({{long_name, "d"}}, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first.size(), 32u);
}

TEST(CommandsDepth, ClampSkipsDuplicateInReserved) {
    auto out = clamp_command_names({{"foo", "d"}}, {"foo"});
    EXPECT_TRUE(out.empty());
}

TEST(CommandsDepth, ClampCollisionWithDigitSuffix) {
    std::string long1(40, 'x');
    std::string long2(40, 'x');
    // Same 40-char name in both; reserved has the 32-char prefix.
    auto out = clamp_command_names({{long1, "d1"}, {long2, "d2"}},
                                   {std::string(32, 'x')});
    // First entry: 32-char prefix collides → '0' suffix.
    // Second entry: also collides → '1' suffix (0 now used).
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, std::string(31, 'x') + "0");
    EXPECT_EQ(out[1].first, std::string(31, 'x') + "1");
}

TEST(CommandsDepth, ClampExhaustedDigitsDrops) {
    std::string long_name(40, 'y');
    std::unordered_set<std::string> reserved;
    reserved.insert(std::string(32, 'y'));
    for (int d = 0; d < 10; ++d) {
        reserved.insert(std::string(31, 'y') + static_cast<char>('0' + d));
    }
    auto out = clamp_command_names({{long_name, "d"}}, reserved);
    EXPECT_TRUE(out.empty());
}

TEST(CommandsDepth, BuildDescriptionWithArgsHint) {
    auto c = mk("title", "Set a title", "[name]");
    EXPECT_EQ(build_description(c),
              "Set a title (usage: /title [name])");
}

TEST(CommandsDepth, BuildDescriptionWithoutArgsHint) {
    auto c = mk("status", "Show status");
    EXPECT_EQ(build_description(c), "Show status");
}

TEST(CommandsDepth, IsGatewayAvailableDefault) {
    EXPECT_TRUE(is_gateway_available(mk("status", "x")));
}

TEST(CommandsDepth, IsGatewayAvailableCliOnly) {
    auto c = mk("config", "x", "", {}, /*cli_only=*/true);
    EXPECT_FALSE(is_gateway_available(c));
}

TEST(CommandsDepth, IsGatewayAvailableConfigGatedTruthy) {
    auto c = mk("rl", "x", "", {}, true);
    c.gateway_config_gate = "rl.enabled";
    c.gateway_config_gate_truthy = true;
    EXPECT_TRUE(is_gateway_available(c));
}

TEST(CommandsDepth, IsGatewayAvailableConfigGatedFalsy) {
    auto c = mk("rl", "x", "", {}, true);
    c.gateway_config_gate = "rl.enabled";
    c.gateway_config_gate_truthy = false;
    EXPECT_FALSE(is_gateway_available(c));
}

TEST(CommandsDepth, FormatGatewayHelpLineNoArgsNoAlias) {
    auto c = mk("status", "Show session info");
    EXPECT_EQ(format_gateway_help_line(c),
              "`/status` -- Show session info");
}

TEST(CommandsDepth, FormatGatewayHelpLineWithArgs) {
    auto c = mk("title", "Set title", "[name]");
    EXPECT_EQ(format_gateway_help_line(c),
              "`/title [name]` -- Set title");
}

TEST(CommandsDepth, FormatGatewayHelpLineWithAlias) {
    auto c = mk("new", "Start new", "", {"reset"});
    EXPECT_EQ(format_gateway_help_line(c),
              "`/new` -- Start new (alias: `/reset`)");
}

TEST(CommandsDepth, FormatGatewayHelpLineFiltersHyphenNoiseAlias) {
    auto c = mk("reload_mcp", "Reload MCP", "", {"reload-mcp"});
    // alias "reload-mcp" is hyphen-equivalent to canonical → filtered
    EXPECT_EQ(format_gateway_help_line(c),
              "`/reload_mcp` -- Reload MCP");
}

TEST(CommandsDepth, FormatGatewayHelpLineMultipleAliases) {
    auto c = mk("queue", "Queue a prompt", "<prompt>", {"q"});
    EXPECT_EQ(format_gateway_help_line(c),
              "`/queue <prompt>` -- Queue a prompt (alias: `/q`)");
}

TEST(CommandsDepth, TelegramBotCommandsSanitisesAndSkipsInvalid) {
    std::vector<CommandSpec> reg = {
        mk("set-home", "Set home"),
        mk("!!!", "invalid"),           // sanitized → empty → skipped
        mk("status", "Show status"),
    };
    auto out = telegram_bot_commands(reg);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, "set_home");
    EXPECT_EQ(out[1].first, "status");
}

TEST(CommandsDepth, TelegramBotCommandsSkipsCliOnly) {
    std::vector<CommandSpec> reg = {
        mk("config", "cli only", "", {}, /*cli_only=*/true),
        mk("status", "public"),
    };
    auto out = telegram_bot_commands(reg);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, "status");
}

TEST(CommandsDepth, ExtractPipeSubcommandsThreeOptions) {
    auto out = extract_pipe_subcommands("[on|off|status]");
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->size(), 3u);
    EXPECT_EQ((*out)[0], "on");
    EXPECT_EQ((*out)[1], "off");
    EXPECT_EQ((*out)[2], "status");
}

TEST(CommandsDepth, ExtractPipeSubcommandsNoMatch) {
    EXPECT_FALSE(extract_pipe_subcommands("[name]").has_value());
    EXPECT_FALSE(extract_pipe_subcommands("<prompt>").has_value());
    EXPECT_FALSE(extract_pipe_subcommands("").has_value());
}

TEST(CommandsDepth, ExtractPipeSubcommandsTwoOptions) {
    auto out = extract_pipe_subcommands("[tts|status]");
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->size(), 2u);
    EXPECT_EQ((*out)[0], "tts");
    EXPECT_EQ((*out)[1], "status");
}

TEST(CommandsDepth, FilterAliasNoiseIdentityDropped) {
    auto out = filter_alias_noise("new", {"new", "reset"});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "reset");
}

TEST(CommandsDepth, FilterAliasNoiseHyphenEquivalent) {
    auto out = filter_alias_noise("reload_mcp", {"reload-mcp", "mcp"});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "mcp");
}

TEST(CommandsDepth, FilterAliasNoisePreservesRealAliases) {
    auto out = filter_alias_noise("new", {"reset", "fresh"});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "reset");
    EXPECT_EQ(out[1], "fresh");
}
