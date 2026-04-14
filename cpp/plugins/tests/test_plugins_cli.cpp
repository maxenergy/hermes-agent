// Unit tests for `hermes plugins ...` CLI: URL resolution, path sanitization,
// manifest parsing, state persistence, capability enforcement, install /
// uninstall / enable / disable / info / search / list.
//
// Tests avoid network calls — git clone paths are exercised through the
// local-path install branch.

#include "hermes/plugins/manifest.hpp"
#include "hermes/plugins/plugin_manager.hpp"
#include "hermes/plugins/plugins_cli.hpp"
#include "hermes/plugins/state.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace hp = hermes::plugins;

class PluginsCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("hermes_plugins_cli_test_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmp_);
        plugins_dir_ = tmp_ / "plugins";
        fs::create_directories(plugins_dir_);
        state_path_ = plugins_dir_ / "state.json";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    void write_manifest(const std::string& name,
                        const std::string& yaml_body) {
        auto dir = plugins_dir_ / name;
        fs::create_directories(dir);
        std::ofstream(dir / "plugin.yaml") << yaml_body;
    }

    hp::PluginsOptions opts() const {
        hp::PluginsOptions o;
        o.plugins_dir = plugins_dir_;
        o.state_path  = state_path_;
        o.quiet       = true;
        return o;
    }

    fs::path tmp_;
    fs::path plugins_dir_;
    fs::path state_path_;
};

// 1. resolve_git_url: owner/repo → GitHub HTTPS.
TEST_F(PluginsCliTest, ResolveGitUrlShorthand) {
    EXPECT_EQ(hp::resolve_git_url("maxenergy/foo"),
              "https://github.com/maxenergy/foo.git");
}

// 2. resolve_git_url: passthrough of full URL.
TEST_F(PluginsCliTest, ResolveGitUrlPassthrough) {
    EXPECT_EQ(hp::resolve_git_url("https://gitlab.com/x/y.git"),
              "https://gitlab.com/x/y.git");
    EXPECT_EQ(hp::resolve_git_url("git@github.com:x/y.git"),
              "git@github.com:x/y.git");
}

// 3. resolve_git_url: rejects malformed input.
TEST_F(PluginsCliTest, ResolveGitUrlRejectsMalformed) {
    EXPECT_THROW(hp::resolve_git_url(""), std::invalid_argument);
    EXPECT_THROW(hp::resolve_git_url("just-a-name"), std::invalid_argument);
}

// 4. repo_name_from_url: strips .git + path.
TEST_F(PluginsCliTest, RepoNameFromUrl) {
    EXPECT_EQ(hp::repo_name_from_url("https://github.com/foo/bar.git"), "bar");
    EXPECT_EQ(hp::repo_name_from_url("git@github.com:foo/bar.git"), "bar");
    EXPECT_EQ(hp::repo_name_from_url("https://example.com/baz/"), "baz");
}

// 5. sanitize_plugin_name: rejects traversal + slashes + empty.
TEST_F(PluginsCliTest, SanitizePluginNameRejectsBadInput) {
    EXPECT_THROW(hp::sanitize_plugin_name("", plugins_dir_),
                 std::invalid_argument);
    EXPECT_THROW(hp::sanitize_plugin_name("..", plugins_dir_),
                 std::invalid_argument);
    EXPECT_THROW(hp::sanitize_plugin_name("foo/bar", plugins_dir_),
                 std::invalid_argument);
    EXPECT_THROW(hp::sanitize_plugin_name("../evil", plugins_dir_),
                 std::invalid_argument);
}

// 6. sanitize_plugin_name: valid name → target inside plugins_dir.
TEST_F(PluginsCliTest, SanitizePluginNameAccepts) {
    auto target = hp::sanitize_plugin_name("ok_name", plugins_dir_);
    EXPECT_EQ(target.filename().string(), "ok_name");
    EXPECT_EQ(target.parent_path(), fs::weakly_canonical(plugins_dir_));
}

// 7. Manifest parse: basic fields + capabilities.
TEST_F(PluginsCliTest, ManifestParse) {
    write_manifest("demo", R"(
name: demo
version: 1.2.3
description: A demo plugin
author: rogers
manifest_version: 1
requires_env:
  - MY_API_KEY
provides_tools:
  - demo_tool
capabilities:
  tools: [demo_tool, other_tool]
  commands: ["/demo"]
  events: [pre_tool_call]
  cli: [demo]
)");
    auto m = hp::load_manifest(plugins_dir_ / "demo" / "plugin.yaml");
    EXPECT_EQ(m.name, "demo");
    EXPECT_EQ(m.version, "1.2.3");
    EXPECT_EQ(m.description, "A demo plugin");
    EXPECT_EQ(m.manifest_version, 1);
    ASSERT_EQ(m.requires_env.size(), 1u);
    EXPECT_EQ(m.requires_env[0], "MY_API_KEY");
    EXPECT_EQ(m.capabilities.tools.size(), 2u);
}

// 8. Capability: allows / denies correctly, wildcard works.
TEST_F(PluginsCliTest, CapabilitiesAllows) {
    hp::Capabilities caps;
    caps.tools = {"ok_tool"};
    EXPECT_TRUE(caps.allows_tool("ok_tool"));
    EXPECT_FALSE(caps.allows_tool("denied"));
    caps.tools = {"*"};
    EXPECT_TRUE(caps.allows_tool("anything"));
    // Empty grant — denies all.
    caps.tools = {};
    EXPECT_FALSE(caps.allows_tool("anything"));
}

// 9. PluginManager::capability_allows dispatches by kind.
TEST_F(PluginsCliTest, ManagerCapabilityDispatch) {
    hp::PluginManifest m;
    m.capabilities.tools    = {"t1"};
    m.capabilities.commands = {"/c1"};
    m.capabilities.events   = {"e1"};
    m.capabilities.cli      = {"x"};
    EXPECT_TRUE(hp::PluginManager::capability_allows(m, "tool", "t1"));
    EXPECT_FALSE(hp::PluginManager::capability_allows(m, "tool", "t2"));
    EXPECT_TRUE(hp::PluginManager::capability_allows(m, "command", "/c1"));
    EXPECT_TRUE(hp::PluginManager::capability_allows(m, "event", "e1"));
    EXPECT_TRUE(hp::PluginManager::capability_allows(m, "cli", "x"));
    EXPECT_FALSE(hp::PluginManager::capability_allows(m, "unknown-kind", "x"));
}

// 10. State: load / save roundtrip.
TEST_F(PluginsCliTest, StateRoundtrip) {
    hp::PluginState s;
    s.add_installed("a");
    s.add_installed("b");
    s.disable("a");
    ASSERT_TRUE(hp::save_state(state_path_, s));

    auto loaded = hp::load_state(state_path_);
    EXPECT_TRUE(loaded.is_installed("a"));
    EXPECT_TRUE(loaded.is_installed("b"));
    EXPECT_TRUE(loaded.is_disabled("a"));
    EXPECT_FALSE(loaded.is_disabled("b"));
    EXPECT_TRUE(loaded.is_enabled("b"));
}

// 11. State: load on nonexistent file returns empty.
TEST_F(PluginsCliTest, StateLoadMissing) {
    auto s = hp::load_state(plugins_dir_ / "does_not_exist.json");
    EXPECT_TRUE(s.installed.empty());
    EXPECT_TRUE(s.disabled.empty());
}

// 12. plugins_list on empty dir succeeds (returns 0).
TEST_F(PluginsCliTest, ListEmpty) {
    EXPECT_EQ(hp::plugins_list(opts()), 0);
}

// 13. plugins_list with manifests finds them.
TEST_F(PluginsCliTest, ListFindsPlugins) {
    write_manifest("alpha", "name: alpha\nversion: 0.1\n");
    write_manifest("beta",  "name: beta\nversion: 2.0\n");
    auto manifests = hp::scan_plugins_dir(plugins_dir_, "user");
    ASSERT_EQ(manifests.size(), 2u);
    EXPECT_EQ(manifests[0].name, "alpha");
    EXPECT_EQ(manifests[1].name, "beta");
}

// 14. install from local path copies into plugins_dir and records state.
TEST_F(PluginsCliTest, InstallFromLocalPath) {
    // Build a source plugin dir in tmp_.
    auto src = tmp_ / "src_plugin";
    fs::create_directories(src);
    std::ofstream(src / "plugin.yaml")
        << "name: src_plugin\nversion: 0.9\n";

    EXPECT_EQ(hp::plugins_install(src.string(), opts()), 0);
    EXPECT_TRUE(fs::exists(plugins_dir_ / "src_plugin" / "plugin.yaml"));

    auto s = hp::load_state(state_path_);
    EXPECT_TRUE(s.is_installed("src_plugin"));
}

// 15. install twice without --force fails.
TEST_F(PluginsCliTest, InstallLocalTwiceWithoutForceFails) {
    auto src = tmp_ / "src_plugin2";
    fs::create_directories(src);
    std::ofstream(src / "plugin.yaml")
        << "name: src_plugin2\n";

    ASSERT_EQ(hp::plugins_install(src.string(), opts()), 0);
    EXPECT_EQ(hp::plugins_install(src.string(), opts()), 1);
}

// 16. install --force overwrites existing.
TEST_F(PluginsCliTest, InstallLocalForceOverwrites) {
    auto src = tmp_ / "src_plugin3";
    fs::create_directories(src);
    std::ofstream(src / "plugin.yaml") << "name: src_plugin3\nversion: 1\n";

    ASSERT_EQ(hp::plugins_install(src.string(), opts()), 0);

    std::ofstream(src / "plugin.yaml") << "name: src_plugin3\nversion: 2\n";
    auto o = opts();
    o.force = true;
    ASSERT_EQ(hp::plugins_install(src.string(), o), 0);

    auto m = hp::load_manifest(plugins_dir_ / "src_plugin3" / "plugin.yaml");
    EXPECT_EQ(m.version, "2");
}

// 17. uninstall removes the directory and clears state.
TEST_F(PluginsCliTest, UninstallRemovesPlugin) {
    write_manifest("to_remove", "name: to_remove\n");
    hp::PluginState s;
    s.add_installed("to_remove");
    hp::save_state(state_path_, s);

    EXPECT_EQ(hp::plugins_uninstall("to_remove", opts()), 0);
    EXPECT_FALSE(fs::exists(plugins_dir_ / "to_remove"));
    auto loaded = hp::load_state(state_path_);
    EXPECT_FALSE(loaded.is_installed("to_remove"));
}

// 18. uninstall on missing plugin returns error.
TEST_F(PluginsCliTest, UninstallMissing) {
    EXPECT_EQ(hp::plugins_uninstall("ghost", opts()), 1);
}

// 19. enable/disable toggle state.
TEST_F(PluginsCliTest, EnableDisableToggle) {
    write_manifest("togl", "name: togl\n");
    EXPECT_EQ(hp::plugins_disable("togl", opts()), 0);
    EXPECT_TRUE(hp::load_state(state_path_).is_disabled("togl"));
    EXPECT_EQ(hp::plugins_enable("togl", opts()), 0);
    EXPECT_FALSE(hp::load_state(state_path_).is_disabled("togl"));
}

// 20. enable on missing plugin fails.
TEST_F(PluginsCliTest, EnableMissing) {
    EXPECT_EQ(hp::plugins_enable("ghost", opts()), 1);
    EXPECT_EQ(hp::plugins_disable("ghost", opts()), 1);
}

// 21. info on known plugin succeeds, missing plugin fails.
TEST_F(PluginsCliTest, InfoSucceedsAndFails) {
    write_manifest("foo", "name: foo\nversion: 0.1\ndescription: hi\n");
    EXPECT_EQ(hp::plugins_info("foo", opts()), 0);
    EXPECT_EQ(hp::plugins_info("missing", opts()), 1);
}

// 22. search returns 0 regardless of match count.
TEST_F(PluginsCliTest, SearchReturnsZero) {
    write_manifest("searchable",
        "name: searchable\ndescription: a searchable thing\n");
    EXPECT_EQ(hp::plugins_search("searchable", opts()), 0);
    EXPECT_EQ(hp::plugins_search("zzzzz", opts()), 0);
    EXPECT_EQ(hp::plugins_search("", opts()), 0);
}

// 23. update on non-git plugin fails with explanatory error.
TEST_F(PluginsCliTest, UpdateNonGitFails) {
    write_manifest("nogit", "name: nogit\n");
    EXPECT_EQ(hp::plugins_update("nogit", opts()), 1);
}

// 24. update on missing plugin fails.
TEST_F(PluginsCliTest, UpdateMissingFails) {
    EXPECT_EQ(hp::plugins_update("ghost", opts()), 1);
}

// 25. reload on missing plugin fails.
TEST_F(PluginsCliTest, ReloadMissingFails) {
    EXPECT_EQ(hp::plugins_reload("ghost", opts()), 1);
}

// 26. PluginManager::scan_manifests picks up manifests.
TEST_F(PluginsCliTest, ManagerScanManifests) {
    write_manifest("mm", "name: mm\nversion: 7\n");
    hp::PluginManager mgr(plugins_dir_);
    auto ms = mgr.scan_manifests();
    ASSERT_EQ(ms.size(), 1u);
    EXPECT_EQ(ms[0].name, "mm");
    EXPECT_EQ(ms[0].version, "7");
}

// 27. discover respects state.json disabled list.
TEST_F(PluginsCliTest, DiscoverRespectsDisabledState) {
    write_manifest("disme", "name: disme\n");
    // State.json alongside plugins_dir (inside the dir).
    hp::PluginState s;
    s.add_installed("disme");
    s.disable("disme");
    hp::save_state(plugins_dir_ / "state.json", s);

    hp::PluginManager mgr(plugins_dir_);
    mgr.discover();
    auto infos = mgr.list();
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].name, "disme");
    EXPECT_FALSE(infos[0].enabled);
}
