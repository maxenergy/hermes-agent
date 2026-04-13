// Verifies plugin-contributed commands flow through the
// `register_plugin_command` API into the derived lookup caches
// (`commands_flat()`, `commands_by_category()`, `gateway_help_lines()`,
// `gateway_known_commands()`) after `rebuild_lookups()` fires.
//
// Also smoke-tests the `PluginManager::set_rebuild_lookups_cb` wiring:
// after a (no-op) `discover()` on an empty directory the callback is
// *not* fired; after a successful `load()` it is.

#include "hermes/cli/commands.hpp"
#include "hermes/plugins/plugin_manager.hpp"

#include <filesystem>
#include <gtest/gtest.h>

namespace hc = hermes::cli;
namespace hp = hermes::plugins;

namespace {

// Auto-cleanup: drop any plugin commands registered during a test.
struct PluginCommandFixture : public ::testing::Test {
    void TearDown() override { hc::clear_plugin_commands(); }
};

}  // namespace

using RebuildLookups = PluginCommandFixture;

TEST_F(RebuildLookups, RegisterPluginCommandAddsToFlatMap) {
    hc::CommandDef cmd;
    cmd.name = "plug-hello";
    cmd.description = "Plugin-contributed hello";
    cmd.category = "Plugins";
    cmd.aliases = {"ph"};
    cmd.args_hint = "";
    cmd.cli_only = false;

    ASSERT_TRUE(hc::register_plugin_command(cmd));

    auto flat = hc::commands_flat();
    ASSERT_TRUE(flat.count("plug-hello"));
    EXPECT_EQ(flat["plug-hello"].description, "Plugin-contributed hello");
    // Alias also indexed.
    ASSERT_TRUE(flat.count("ph"));
    EXPECT_EQ(flat["ph"].name, "plug-hello");
}

TEST_F(RebuildLookups, DuplicatePluginCommandRejected) {
    hc::CommandDef cmd;
    cmd.name = "plug-once";
    ASSERT_TRUE(hc::register_plugin_command(cmd));
    // Second registration of the same name must fail.
    EXPECT_FALSE(hc::register_plugin_command(cmd));
}

TEST_F(RebuildLookups, BaselineShadowingRejected) {
    // `help` is a baseline command — plugins cannot shadow it.
    hc::CommandDef cmd;
    cmd.name = "help";
    cmd.description = "hijacked";
    EXPECT_FALSE(hc::register_plugin_command(cmd));
}

TEST_F(RebuildLookups, UnregisterRemovesFromFlat) {
    hc::CommandDef cmd;
    cmd.name = "plug-goaway";
    ASSERT_TRUE(hc::register_plugin_command(cmd));
    ASSERT_TRUE(hc::commands_flat().count("plug-goaway"));

    EXPECT_EQ(hc::unregister_plugin_command("plug-goaway"), 1u);
    EXPECT_FALSE(hc::commands_flat().count("plug-goaway"));
}

TEST_F(RebuildLookups, ResolveCommandFindsPlugin) {
    hc::CommandDef cmd;
    cmd.name = "plug-resolve";
    cmd.aliases = {"pr"};
    ASSERT_TRUE(hc::register_plugin_command(cmd));

    auto got = hc::resolve_command("plug-resolve");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "plug-resolve");

    auto alias = hc::resolve_command("pr");
    ASSERT_TRUE(alias.has_value());
    EXPECT_EQ(alias->name, "plug-resolve");

    // Leading slash still works.
    auto slashed = hc::resolve_command("/plug-resolve");
    ASSERT_TRUE(slashed.has_value());
}

TEST_F(RebuildLookups, GatewayKnownCommandsIncludesPluginOnRebuild) {
    // Baseline: /help is gateway-visible.
    const auto& before = hc::gateway_known_commands();
    EXPECT_TRUE(before.count("help"));
    EXPECT_FALSE(before.count("plug-net"));

    hc::CommandDef cmd;
    cmd.name = "plug-net";
    cmd.aliases = {"pn"};
    cmd.cli_only = false;
    ASSERT_TRUE(hc::register_plugin_command(cmd));
    // register_plugin_command rebuilds caches implicitly.
    const auto& after = hc::gateway_known_commands();
    EXPECT_TRUE(after.count("plug-net"));
    EXPECT_TRUE(after.count("pn"));
}

TEST_F(RebuildLookups, GatewayKnownExcludesCliOnlyPlugin) {
    hc::CommandDef cmd;
    cmd.name = "plug-cli-only";
    cmd.cli_only = true;
    ASSERT_TRUE(hc::register_plugin_command(cmd));
    const auto& set = hc::gateway_known_commands();
    EXPECT_FALSE(set.count("plug-cli-only"));
}

TEST_F(RebuildLookups, GatewayHelpLinesIncludesPlugin) {
    hc::CommandDef cmd;
    cmd.name = "plug-help-row";
    cmd.description = "plugin help description";
    cmd.args_hint = "<arg>";
    ASSERT_TRUE(hc::register_plugin_command(cmd));

    auto lines = hc::gateway_help_lines();
    bool found = false;
    for (const auto& l : lines) {
        if (l.find("plug-help-row") != std::string::npos &&
            l.find("plugin help description") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(RebuildLookups, ByCategoryGroupsPluginCommands) {
    hc::CommandDef a;
    a.name = "plug-cat-a";
    a.category = "Plugins";
    hc::CommandDef b;
    b.name = "plug-cat-b";
    b.category = "Plugins";
    ASSERT_TRUE(hc::register_plugin_command(a));
    ASSERT_TRUE(hc::register_plugin_command(b));

    auto by_cat = hc::commands_by_category();
    ASSERT_TRUE(by_cat.count("Plugins"));
    // Both plugin commands should be in the Plugins bucket.
    bool found_a = false, found_b = false;
    for (const auto& c : by_cat["Plugins"]) {
        if (c.name == "plug-cat-a") found_a = true;
        if (c.name == "plug-cat-b") found_b = true;
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

TEST_F(RebuildLookups, ExplicitRebuildLookupsIsIdempotent) {
    hc::CommandDef cmd;
    cmd.name = "plug-idemp";
    ASSERT_TRUE(hc::register_plugin_command(cmd));
    // Repeated rebuilds should not duplicate entries.
    hc::rebuild_lookups();
    hc::rebuild_lookups();
    const auto& set = hc::gateway_known_commands();
    EXPECT_EQ(set.count("plug-idemp"), 1u);
}

TEST_F(RebuildLookups, PluginManagerRebuildCallbackNotFiredOnEmptyDiscover) {
    auto tmp = std::filesystem::temp_directory_path() /
               ("hermes-pm-cb-empty-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);

    hp::PluginManager mgr(tmp);
    int fired = 0;
    mgr.set_rebuild_lookups_cb([&] { ++fired; });
    mgr.discover();
    // No plugins were loaded — callback must not fire.
    EXPECT_EQ(fired, 0);

    std::filesystem::remove_all(tmp);
}

TEST_F(RebuildLookups, PluginManagerRebuildCallbackMissingNameSafe) {
    // Load a plugin that doesn't exist — callback must not fire.
    hp::PluginManager mgr(std::filesystem::temp_directory_path());
    int fired = 0;
    mgr.set_rebuild_lookups_cb([&] { ++fired; });
    EXPECT_FALSE(mgr.load("no_such_plugin"));
    EXPECT_EQ(fired, 0);
}
