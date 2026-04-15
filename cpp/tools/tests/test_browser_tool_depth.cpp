// Tests for hermes/tools/browser_tool_depth.hpp — pure helpers mirroring
// configuration-layer decisions in tools/browser_tool.py.
#include "hermes/tools/browser_tool_depth.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace hermes::tools::browser_depth;

TEST(BrowserDepthTimeout, MissingUsesDefault) {
    EXPECT_EQ(resolve_command_timeout(std::nullopt), 30);
}

TEST(BrowserDepthTimeout, BelowFloorClamped) {
    EXPECT_EQ(resolve_command_timeout(1), 5);
    EXPECT_EQ(resolve_command_timeout(3), 5);
}

TEST(BrowserDepthTimeout, NormalUsed) {
    EXPECT_EQ(resolve_command_timeout(45), 45);
}

TEST(BrowserDepthTimeout, ZeroFallsBack) {
    EXPECT_EQ(resolve_command_timeout(0, 17), 17);
    EXPECT_EQ(resolve_command_timeout(-5, 17), 17);
}

TEST(BrowserDepthCdp, ClassifyEmpty) {
    EXPECT_EQ(classify_cdp_endpoint(""), CdpStrategy::Empty);
    EXPECT_EQ(classify_cdp_endpoint("   "), CdpStrategy::Empty);
}

TEST(BrowserDepthCdp, ClassifyPassThrough) {
    EXPECT_EQ(classify_cdp_endpoint(
                  "ws://host:9222/devtools/browser/abc-def"),
              CdpStrategy::PassThroughWebsocket);
}

TEST(BrowserDepthCdp, ClassifyBareWs) {
    EXPECT_EQ(classify_cdp_endpoint("ws://host:9222"),
              CdpStrategy::WebsocketDiscovery);
    EXPECT_EQ(classify_cdp_endpoint("wss://host:9222/"),
              CdpStrategy::WebsocketDiscovery);
}

TEST(BrowserDepthCdp, ClassifyHttp) {
    EXPECT_EQ(classify_cdp_endpoint("http://host:9222"),
              CdpStrategy::HttpDiscovery);
    EXPECT_EQ(classify_cdp_endpoint("http://host:9222/json/version"),
              CdpStrategy::HttpDiscovery);
}

TEST(BrowserDepthCdp, BuildDiscoveryAppendsPath) {
    EXPECT_EQ(build_cdp_discovery_url("http://host:9222"),
              "http://host:9222/json/version");
    EXPECT_EQ(build_cdp_discovery_url("http://host:9222/"),
              "http://host:9222/json/version");
}

TEST(BrowserDepthCdp, BuildDiscoveryUpgradesWs) {
    EXPECT_EQ(build_cdp_discovery_url("ws://host:9222"),
              "http://host:9222/json/version");
    EXPECT_EQ(build_cdp_discovery_url("wss://host:9222"),
              "https://host:9222/json/version");
}

TEST(BrowserDepthCdp, BuildDiscoveryKeepsExistingSuffix) {
    EXPECT_EQ(build_cdp_discovery_url("http://host:9222/json/version"),
              "http://host:9222/json/version");
}

TEST(BrowserDepthCdp, PickWebsocketReturnsField) {
    std::string resp = R"({"webSocketDebuggerUrl":"ws://host:9222/devtools/browser/abc"})";
    EXPECT_EQ(pick_websocket_from_discovery("http://host:9222", resp),
              "ws://host:9222/devtools/browser/abc");
}

TEST(BrowserDepthCdp, PickWebsocketMissingField) {
    EXPECT_EQ(pick_websocket_from_discovery("http://x", "{}"), "http://x");
}

TEST(BrowserDepthCdp, PickWebsocketInvalidJson) {
    EXPECT_EQ(pick_websocket_from_discovery("http://x", "not-json"),
              "http://x");
}

TEST(BrowserDepthProvider, KnownNames) {
    EXPECT_TRUE(is_known_cloud_provider("browserbase"));
    EXPECT_TRUE(is_known_cloud_provider("browser-use"));
    EXPECT_TRUE(is_known_cloud_provider("firecrawl"));
    EXPECT_FALSE(is_known_cloud_provider("foo"));
}

TEST(BrowserDepthProvider, NormaliseCanonical) {
    EXPECT_EQ(normalise_cloud_provider_name(" Browserbase "), "browserbase");
    EXPECT_EQ(normalise_cloud_provider_name("browser-use"), "browser-use");
    EXPECT_EQ(normalise_cloud_provider_name("firecrawl"), "firecrawl");
}

TEST(BrowserDepthProvider, NormaliseAliases) {
    EXPECT_EQ(normalise_cloud_provider_name("browseruse"), "browser-use");
    EXPECT_EQ(normalise_cloud_provider_name("browser_use"), "browser-use");
    EXPECT_EQ(normalise_cloud_provider_name("BB"), "browserbase");
    EXPECT_EQ(normalise_cloud_provider_name("none"), "local");
    EXPECT_EQ(normalise_cloud_provider_name("OFF"), "local");
    EXPECT_EQ(normalise_cloud_provider_name("disabled"), "local");
    EXPECT_EQ(normalise_cloud_provider_name("local"), "local");
}

TEST(BrowserDepthProvider, NormaliseUnknown) {
    EXPECT_EQ(normalise_cloud_provider_name("mystery"), "");
    EXPECT_EQ(normalise_cloud_provider_name(""), "");
}

TEST(BrowserDepthInstall, LinuxHint) {
    EXPECT_NE(browser_install_hint(false).find("--with-deps"),
              std::string::npos);
}

TEST(BrowserDepthInstall, TermuxHint) {
    EXPECT_EQ(browser_install_hint(true).find("--with-deps"),
              std::string::npos);
}

TEST(BrowserDepthInstall, TermuxError) {
    const auto msg = termux_browser_install_error();
    EXPECT_NE(msg.find("Termux"), std::string::npos);
    EXPECT_NE(msg.find("npm install"), std::string::npos);
}

TEST(BrowserDepthInstall, RequiresRealTermuxInstall) {
    EXPECT_TRUE(requires_real_termux_browser_install("npx agent-browser", true,
                                                     true));
    EXPECT_FALSE(requires_real_termux_browser_install("npx agent-browser",
                                                      false, true));
    EXPECT_FALSE(requires_real_termux_browser_install("npx agent-browser", true,
                                                      false));
    EXPECT_FALSE(requires_real_termux_browser_install("agent-browser", true,
                                                      true));
}

TEST(BrowserDepthSsrf, AppliesForCloud) {
    EXPECT_TRUE(ssrf_protection_applies(false, true));
}

TEST(BrowserDepthSsrf, SkipsForLocal) {
    EXPECT_FALSE(ssrf_protection_applies(false, false));
    EXPECT_FALSE(ssrf_protection_applies(true, false));
    EXPECT_FALSE(ssrf_protection_applies(true, true));
}

TEST(BrowserDepthSsrf, AllowPrivateDefaultFalse) {
    EXPECT_FALSE(resolve_allow_private_urls(std::nullopt));
    EXPECT_FALSE(resolve_allow_private_urls(false));
    EXPECT_TRUE(resolve_allow_private_urls(true));
}

TEST(BrowserDepthTmpdir, MacUsesTmp) {
    EXPECT_EQ(resolve_browser_tmpdir("darwin", "/var/folders/xy/T"), "/tmp");
}

TEST(BrowserDepthTmpdir, LinuxPassthrough) {
    EXPECT_EQ(resolve_browser_tmpdir("linux", "/tmp"), "/tmp");
    EXPECT_EQ(resolve_browser_tmpdir("linux", "/mnt/tmp"), "/mnt/tmp");
}

TEST(BrowserDepthCleanup, FirstCallDue) {
    EXPECT_TRUE(screenshot_cleanup_due(0.0, 1000.0));
}

TEST(BrowserDepthCleanup, RecentNotDue) {
    EXPECT_FALSE(screenshot_cleanup_due(950.0, 1000.0, 60));
}

TEST(BrowserDepthCleanup, OldDue) {
    EXPECT_TRUE(screenshot_cleanup_due(900.0, 1000.0, 60));
    EXPECT_TRUE(screenshot_cleanup_due(800.0, 1000.0, 60));
}

TEST(BrowserDepthPrompt, TaskAwareIncludesTask) {
    auto p = build_snapshot_extraction_prompt("SNAPSHOT", "find checkout");
    EXPECT_NE(p.find("find checkout"), std::string::npos);
    EXPECT_NE(p.find("Interactive elements"), std::string::npos);
    EXPECT_NE(p.find("SNAPSHOT"), std::string::npos);
}

TEST(BrowserDepthPrompt, GenericVariant) {
    auto p = build_snapshot_extraction_prompt("S", "");
    EXPECT_NE(p.find("Summarize this page snapshot"), std::string::npos);
    EXPECT_EQ(p.find("user's task"), std::string::npos);
}

TEST(BrowserDepthEmptyOk, CloseAndRecord) {
    EXPECT_TRUE(is_empty_ok_command("close"));
    EXPECT_TRUE(is_empty_ok_command("record"));
    EXPECT_FALSE(is_empty_ok_command("click"));
    EXPECT_FALSE(is_empty_ok_command(""));
}

TEST(BrowserDepthHomebrew, FilterNodeDirs) {
    auto out = filter_homebrew_node_dirs(
        {"node", "node@20", "node@24", "python@3.12", "nodejs"});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out.at(0), "node@20");
    EXPECT_EQ(out.at(1), "node@24");
    EXPECT_EQ(out.at(2), "nodejs");
}

TEST(BrowserDepthHomebrew, FilterEmpty) {
    EXPECT_TRUE(filter_homebrew_node_dirs({}).empty());
    EXPECT_TRUE(filter_homebrew_node_dirs({"node"}).empty());
}

TEST(BrowserDepthEnv, TrimEnvValue) {
    EXPECT_EQ(trim_env_value("  model-name  "), "model-name");
    EXPECT_EQ(trim_env_value(""), "");
}
