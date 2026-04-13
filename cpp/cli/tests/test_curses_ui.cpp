// Headless tests for curses_ui: MenuState / TableState state machines.
#include "hermes/cli/curses_ui.hpp"

#include <gtest/gtest.h>
#include <sstream>

namespace hermes::cli::curses_ui {
namespace {

MenuState make_menu() {
    MenuState m;
    m.title = "pick a tool";
    m.items.push_back({"web_search", "web_search", true, "[on]"});
    m.items.push_back({"file_read", "file_read", true, "[off]"});
    m.items.push_back({"dangerous", "dangerous", false, "[disabled]"});
    return m;
}

TEST(MenuStateTest, MoveDownSkipsDisabled) {
    auto m = make_menu();
    EXPECT_EQ(m.selected, 0u);
    m.move_down();
    EXPECT_EQ(m.selected, 1u);
    // Next move_down should skip the disabled item (index 2) and wrap to 0.
    m.move_down();
    EXPECT_EQ(m.selected, 0u);
}

TEST(MenuStateTest, MoveUpSkipsDisabled) {
    auto m = make_menu();
    m.selected = 1;
    m.move_up();
    EXPECT_EQ(m.selected, 0u);
    // From 0, moving up wraps -> 2 is disabled -> skip -> 1.
    m.move_up();
    EXPECT_EQ(m.selected, 1u);
}

TEST(MenuStateTest, SelectIgnoresDisabled) {
    auto m = make_menu();
    m.selected = 2;
    m.select_current();
    EXPECT_FALSE(m.done);  // disabled => not done
}

TEST(MenuStateTest, RenderMarksSelectedAndPadsToWidth) {
    auto m = make_menu();
    auto lines = m.render(40);
    ASSERT_GE(lines.size(), 3u);
    for (const auto& line : lines) {
        EXPECT_EQ(line.size(), 40u);
        EXPECT_EQ(line.find("\033[K"), std::string::npos);
    }
    // First line after title+sep is selected (">" marker).
    // Title + sep = 2 lines, then items.
    EXPECT_NE(lines[2].find("> "), std::string::npos);
}

TEST(MenuPlaintextTest, AcceptsNumericSelection) {
    auto m = make_menu();
    std::istringstream in("1\n");
    std::ostringstream out;
    auto idx = run_menu_plaintext(m, out, in);
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1u);
    EXPECT_TRUE(m.done);
    EXPECT_FALSE(m.cancelled);
}

TEST(MenuPlaintextTest, CancelsOnQ) {
    auto m = make_menu();
    std::istringstream in("q\n");
    std::ostringstream out;
    auto idx = run_menu_plaintext(m, out, in);
    EXPECT_FALSE(idx.has_value());
    EXPECT_TRUE(m.cancelled);
}

TEST(MenuPlaintextTest, RejectsDisabledPick) {
    auto m = make_menu();
    std::istringstream in("2\n");
    std::ostringstream out;
    auto idx = run_menu_plaintext(m, out, in);
    EXPECT_FALSE(idx.has_value());
    EXPECT_TRUE(m.cancelled);
}

TEST(TableStateTest, ColumnWidthsFromData) {
    TableState t;
    t.headers = {"name", "status"};
    t.rows.push_back({{"alpha", "on"}, "alpha"});
    t.rows.push_back({{"longer_name", "off"}, "longer_name"});
    auto widths = t.column_widths(80);
    ASSERT_EQ(widths.size(), 2u);
    EXPECT_EQ(widths[0], 11u);  // "longer_name"
    EXPECT_EQ(widths[1], 6u);   // "status"
}

TEST(TableStateTest, RenderPadsAndHonorsWidth) {
    TableState t;
    t.headers = {"n", "s"};
    t.rows.push_back({{"a", "on"}, "a"});
    auto lines = t.render(20);
    for (const auto& l : lines) {
        EXPECT_EQ(l.size(), 20u);
        EXPECT_EQ(l.find("\033[K"), std::string::npos);
    }
}

TEST(TableStateTest, RenderPlaintextIncludesAllRows) {
    TableState t;
    t.headers = {"k", "v"};
    t.rows.push_back({{"a", "1"}, "a"});
    t.rows.push_back({{"b", "2"}, "b"});
    std::ostringstream out;
    render_table_plaintext(t, out, 20);
    const auto s = out.str();
    EXPECT_NE(s.find("k"), std::string::npos);
    EXPECT_NE(s.find("a"), std::string::npos);
    EXPECT_NE(s.find("b"), std::string::npos);
}

}  // namespace
}  // namespace hermes::cli::curses_ui
