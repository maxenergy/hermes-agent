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

}  // namespace
