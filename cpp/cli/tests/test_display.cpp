#include "hermes/cli/display.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli;

TEST(Display, BuildToolPreviewNonEmpty) {
    SkinConfig skin;
    nlohmann::json args = {{"command", "ls -la"}};
    auto preview = build_tool_preview("bash", args, "file1\nfile2", skin);
    EXPECT_FALSE(preview.empty());
    EXPECT_NE(preview.find("bash"), std::string::npos);
}

TEST(Display, GetToolEmojiReturnsString) {
    auto emoji = get_tool_emoji("bash");
    EXPECT_FALSE(emoji.empty());
    auto emoji2 = get_tool_emoji("unknown_tool");
    EXPECT_FALSE(emoji2.empty());  // fallback
}

TEST(Display, SpinnerStartStopLifecycle) {
    SkinConfig skin;
    Spinner spinner(skin);
    EXPECT_NO_THROW({
        spinner.start("testing");
        spinner.update("still testing");
        spinner.stop();
    });
}

TEST(Display, BuildToolPreviewEmptyArgs) {
    SkinConfig skin;
    nlohmann::json args = nlohmann::json::object();
    auto preview = build_tool_preview("read_file", args, "", skin);
    EXPECT_FALSE(preview.empty());
    EXPECT_NE(preview.find("read_file"), std::string::npos);
}

TEST(Display, GetToolEmojiKnownTools) {
    EXPECT_FALSE(get_tool_emoji("bash").empty());
    EXPECT_FALSE(get_tool_emoji("read_file").empty());
    EXPECT_FALSE(get_tool_emoji("search").empty());
    EXPECT_FALSE(get_tool_emoji("memory").empty());
    EXPECT_FALSE(get_tool_emoji("todo").empty());
}
