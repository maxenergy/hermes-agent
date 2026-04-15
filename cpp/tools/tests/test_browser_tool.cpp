#include "hermes/tools/browser_backend.hpp"
#include "hermes/tools/browser_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>

using namespace hermes::tools;

namespace {

class BrowserToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        auto fb = std::make_unique<FakeBrowserBackend>();
        fake_ = fb.get();
        set_browser_backend(std::move(fb));
        register_browser_tools();
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        set_browser_backend(nullptr);
    }

    std::string dispatch(const std::string& name,
                         const nlohmann::json& args = {}) {
        return ToolRegistry::instance().dispatch(name, args, {});
    }

    FakeBrowserBackend* fake_ = nullptr;
};

// 1. navigate records URL in action_log
TEST_F(BrowserToolTest, NavigateRecordsUrl) {
    auto r = nlohmann::json::parse(dispatch("browser_navigate", {{"url", "https://example.com"}}));
    EXPECT_TRUE(r["navigated"].get<bool>());
    EXPECT_EQ(r["url"], "https://example.com");
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "navigate:https://example.com");
}

// 2. snapshot returns fake elements
TEST_F(BrowserToolTest, SnapshotReturnsFakeElements) {
    fake_->fake_snapshot.url = "https://test.com";
    fake_->fake_snapshot.title = "Test Page";
    fake_->fake_snapshot.elements = {{"e1", "div", "Hello", "button", 0, 0, 0, 0, true, {}}};

    auto r = nlohmann::json::parse(dispatch("browser_snapshot"));
    EXPECT_EQ(r["url"], "https://test.com");
    EXPECT_EQ(r["title"], "Test Page");
    ASSERT_EQ(r["elements"].size(), 1u);
    EXPECT_EQ(r["elements"][0]["ref"], "e1");
    EXPECT_EQ(r["elements"][0]["tag"], "div");
    EXPECT_EQ(r["elements"][0]["text"], "Hello");
    EXPECT_EQ(r["elements"][0]["role"], "button");
}

// 3. click with ref logs action
TEST_F(BrowserToolTest, ClickLogsAction) {
    auto r = nlohmann::json::parse(dispatch("browser_click", {{"ref", "btn1"}}));
    EXPECT_TRUE(r["clicked"].get<bool>());
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "click:btn1");
}

// 4. type with submit logs action
TEST_F(BrowserToolTest, TypeWithSubmitLogsAction) {
    auto r = nlohmann::json::parse(
        dispatch("browser_type", {{"ref", "input1"}, {"text", "hello"}, {"submit", true}}));
    EXPECT_TRUE(r["typed"].get<bool>());
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "type:input1:hello:submit");
}

// 5. scroll up/down
TEST_F(BrowserToolTest, ScrollUpDown) {
    dispatch("browser_scroll", {{"direction", "up"}});
    dispatch("browser_scroll", {{"direction", "down"}});
    ASSERT_EQ(fake_->action_log.size(), 2u);
    EXPECT_EQ(fake_->action_log[0], "scroll:up");
    EXPECT_EQ(fake_->action_log[1], "scroll:down");
}

// 6. back logs action
TEST_F(BrowserToolTest, BackLogsAction) {
    auto r = nlohmann::json::parse(dispatch("browser_back"));
    EXPECT_TRUE(r["navigated_back"].get<bool>());
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "go_back");
}

// 7. press_key "Enter"
TEST_F(BrowserToolTest, PressKeyEnter) {
    auto r = nlohmann::json::parse(dispatch("browser_press", {{"key", "Enter"}}));
    EXPECT_TRUE(r["pressed"].get<bool>());
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "press_key:Enter");
}

// 8. get_images returns fake URLs
TEST_F(BrowserToolTest, GetImagesReturnsFakeUrls) {
    fake_->fake_image_urls = {"img1.png", "img2.jpg"};
    auto r = nlohmann::json::parse(dispatch("browser_get_images"));
    ASSERT_EQ(r["images"].size(), 2u);
    EXPECT_EQ(r["images"][0], "img1.png");
    EXPECT_EQ(r["images"][1], "img2.jpg");
}

// 9. console returns fake result
TEST_F(BrowserToolTest, ConsoleReturnsFakeResult) {
    fake_->fake_console_result = {"42", false};
    auto r = nlohmann::json::parse(
        dispatch("browser_console", {{"expression", "1+1"}}));
    EXPECT_EQ(r["value"], "42");
    EXPECT_FALSE(r["is_error"].get<bool>());
}

// 10. no backend -> tool_error for each tool
TEST_F(BrowserToolTest, NoBackendReturnsError) {
    set_browser_backend(nullptr);
    // Re-register so check_fn reflects the nullptr state.
    ToolRegistry::instance().clear();
    register_browser_tools();

    std::vector<std::string> tools = {
        "browser_navigate", "browser_snapshot", "browser_click",
        "browser_type", "browser_scroll", "browser_back",
        "browser_press", "browser_get_images", "browser_vision",
        "browser_console"
    };
    for (const auto& name : tools) {
        nlohmann::json args;
        if (name == "browser_navigate") args["url"] = "x";
        else if (name == "browser_click") args["ref"] = "x";
        else if (name == "browser_type") { args["ref"] = "x"; args["text"] = "x"; }
        else if (name == "browser_scroll") args["direction"] = "up";
        else if (name == "browser_press") args["key"] = "x";
        else if (name == "browser_vision") args["prompt"] = "x";
        else if (name == "browser_console") args["expression"] = "x";

        auto r = nlohmann::json::parse(dispatch(name, args));
        EXPECT_TRUE(r.contains("error")) << "Expected error for " << name;
    }
}

// 11. browser_vision returns screenshot size
TEST_F(BrowserToolTest, VisionReturnsScreenshotSize) {
    fake_->fake_screenshot = "AAABBBCCC";  // 9 bytes
    auto r = nlohmann::json::parse(
        dispatch("browser_vision", {{"prompt", "describe page"}}));
    EXPECT_EQ(r["analysis"], "vision not wired");
    EXPECT_EQ(r["screenshot_size"], 9);
}

// 12. dbl_click flag passed correctly
TEST_F(BrowserToolTest, DblClickFlagPassedCorrectly) {
    dispatch("browser_click", {{"ref", "btn2"}, {"dbl_click", true}});
    ASSERT_EQ(fake_->action_log.size(), 1u);
    EXPECT_EQ(fake_->action_log[0], "click:btn2:dbl");
}

// 13. check_fn returns false when no backend (tools excluded from definitions)
TEST_F(BrowserToolTest, CheckFnExcludesFromDefinitions) {
    set_browser_backend(nullptr);
    auto defs = ToolRegistry::instance().get_definitions();
    for (const auto& d : defs) {
        EXPECT_FALSE(d.name.find("browser_") == 0)
            << "Tool " << d.name << " should not appear without backend";
    }
}

// ---- helper-level tests ---------------------------------------------------

TEST(BrowserToolHelpers, SecretMarkerDetectsAnthropicKey) {
    EXPECT_TRUE(url_contains_secret_marker(
        "https://evil.example.com/?token=sk-ant-AAAAA"));
}

TEST(BrowserToolHelpers, SecretMarkerDetectsUrlEncodedKey) {
    EXPECT_TRUE(url_contains_secret_marker(
        "https://evil.example.com/?token=sk%2Dant%2DAAA"));
}

TEST(BrowserToolHelpers, SecretMarkerDetectsAwsKey) {
    EXPECT_TRUE(url_contains_secret_marker(
        "https://example.com/?aws_key=AKIAIOSFODNN7EXAMPLE"));
}

TEST(BrowserToolHelpers, SecretMarkerCleanUrlPasses) {
    EXPECT_FALSE(url_contains_secret_marker("https://example.com/page"));
}

TEST(BrowserToolHelpers, PrivateAddressDetectsLocalhost) {
    EXPECT_TRUE(url_targets_private_address("http://localhost:8080/x"));
    EXPECT_TRUE(url_targets_private_address("http://127.0.0.1/x"));
}

TEST(BrowserToolHelpers, PrivateAddressDetectsRfc1918) {
    EXPECT_TRUE(url_targets_private_address("http://10.0.0.1/"));
    EXPECT_TRUE(url_targets_private_address("http://192.168.1.1/"));
    EXPECT_TRUE(url_targets_private_address("http://172.16.0.1/"));
    EXPECT_TRUE(url_targets_private_address("http://172.31.255.255/"));
}

TEST(BrowserToolHelpers, PrivateAddressIgnoresPublicRanges) {
    EXPECT_FALSE(url_targets_private_address("http://172.32.0.1/"));
    EXPECT_FALSE(url_targets_private_address("http://8.8.8.8/"));
}

TEST(BrowserToolHelpers, PrivateAddressDetectsMetadataIp) {
    EXPECT_TRUE(url_targets_private_address("http://169.254.169.254/latest"));
}

TEST(BrowserToolHelpers, PrivateAddressDetectsMdns) {
    EXPECT_TRUE(url_targets_private_address("http://router.local/"));
}

TEST(BrowserToolHelpers, PrivateAddressDetectsIpv6Loopback) {
    EXPECT_TRUE(url_targets_private_address("http://[::1]:8080/"));
}

TEST(BrowserToolHelpers, IsSafeBrowserUrlComposes) {
    EXPECT_TRUE(is_safe_browser_url("https://example.com/"));
    EXPECT_FALSE(is_safe_browser_url("http://localhost/"));
    EXPECT_FALSE(is_safe_browser_url("https://x?token=sk-ant-bad"));
    EXPECT_FALSE(is_safe_browser_url(""));
}

TEST(BrowserToolHelpers, BotDetectionPatternsExist) {
    EXPECT_FALSE(bot_detection_title_patterns().empty());
}

TEST(BrowserToolHelpers, LooksLikeBotDetectionMatches) {
    EXPECT_TRUE(looks_like_bot_detection("Just a moment..."));
    EXPECT_TRUE(looks_like_bot_detection("Cloudflare CAPTCHA"));
    EXPECT_FALSE(looks_like_bot_detection("Welcome to Example"));
}

TEST(BrowserToolHelpers, TruncateSnapshotShortReturnsInput) {
    std::string s = "hello\nworld";
    EXPECT_EQ(truncate_snapshot(s, 100), s);
}

TEST(BrowserToolHelpers, TruncateSnapshotCutsAtLineBoundary) {
    std::string s;
    for (int i = 0; i < 200; ++i) s += "line of accessible content\n";
    auto out = truncate_snapshot(s, 500);
    EXPECT_LT(out.size(), 600u);
    EXPECT_NE(out.find("more lines truncated"), std::string::npos);
}

TEST(BrowserToolHelpers, ExtractScreenshotPathQuoted) {
    auto p = extract_screenshot_path(R"(Screenshot saved to "/tmp/x.png")");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "/tmp/x.png");
}

TEST(BrowserToolHelpers, ExtractScreenshotPathUnquoted) {
    auto p = extract_screenshot_path("Screenshot saved to /var/tmp/y.png\n");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "/var/tmp/y.png");
}

TEST(BrowserToolHelpers, ExtractScreenshotPathBareToken) {
    auto p = extract_screenshot_path("hello world /tmp/snap.png ");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "/tmp/snap.png");
}

TEST(BrowserToolHelpers, ExtractScreenshotPathMissing) {
    EXPECT_FALSE(extract_screenshot_path("nothing here").has_value());
}

TEST(BrowserToolHelpers, NormaliseCdpEndpointPassthroughForBrowserPath) {
    auto out = normalise_cdp_endpoint("ws://h:9222/devtools/browser/abc");
    EXPECT_EQ(out, "ws://h:9222/devtools/browser/abc");
}

TEST(BrowserToolHelpers, NormaliseCdpEndpointDropsTrailingSlash) {
    auto out = normalise_cdp_endpoint("http://localhost:9222/");
    EXPECT_EQ(out, "http://localhost:9222");
}

TEST(BrowserToolHelpers, NormaliseCdpEndpointWsToHttp) {
    auto out = normalise_cdp_endpoint("ws://localhost:9222");
    EXPECT_EQ(out, "http://localhost:9222");
}

TEST(BrowserToolHelpers, NormaliseCdpEndpointEmpty) {
    EXPECT_TRUE(normalise_cdp_endpoint("   ").empty());
}

TEST(BrowserToolHelpers, NormaliseBrowserRefStripsBracket) {
    EXPECT_EQ(normalise_browser_ref("[ref=e7]"), "e7");
}

TEST(BrowserToolHelpers, NormaliseBrowserRefRawIsKept) {
    EXPECT_EQ(normalise_browser_ref("e9"), "e9");
}

TEST(BrowserToolHelpers, NormaliseBrowserRefTrimsWhitespace) {
    EXPECT_EQ(normalise_browser_ref("  abc  "), "abc");
}

TEST(BrowserToolHelpers, IsKnownBrowserKeyEnter) {
    EXPECT_TRUE(is_known_browser_key("Enter"));
}

TEST(BrowserToolHelpers, IsKnownBrowserKeySingleChar) {
    EXPECT_TRUE(is_known_browser_key("a"));
}

TEST(BrowserToolHelpers, IsKnownBrowserKeyChord) {
    EXPECT_TRUE(is_known_browser_key("Ctrl+Shift+A"));
}

TEST(BrowserToolHelpers, IsKnownBrowserKeyEmpty) {
    EXPECT_FALSE(is_known_browser_key(""));
}

TEST(BrowserToolHelpers, ParseBrowserKeyChordSingle) {
    auto k = parse_browser_key_chord("Enter");
    EXPECT_EQ(k.key, "Enter");
    EXPECT_TRUE(k.modifiers.empty());
}

TEST(BrowserToolHelpers, ParseBrowserKeyChordWithModifiers) {
    auto k = parse_browser_key_chord("Ctrl+Shift+A");
    EXPECT_EQ(k.key, "A");
    ASSERT_EQ(k.modifiers.size(), 2u);
    EXPECT_EQ(k.modifiers[0], "ctrl");
    EXPECT_EQ(k.modifiers[1], "shift");
}

TEST(BrowserToolHelpers, ParseBrowserKeyChordEmpty) {
    auto k = parse_browser_key_chord("   ");
    EXPECT_TRUE(k.key.empty());
}

}  // namespace
