// Headless tests for FtxuiEditor — drives the state machine directly and
// inspects the rendered frame. No real terminal required.
#include "hermes/cli/ftxui_editor.hpp"

#include <gtest/gtest.h>

namespace hermes::cli {
namespace {

EditorEvent key(EditorKey k) {
    EditorEvent e;
    e.key = k;
    return e;
}

TEST(FtxuiEditorTest, InsertsCharsAndSubmitsOnEnter) {
    FtxuiEditor ed;
    ed.set_terminal_width(40);
    ed.type("hello");
    EXPECT_EQ(ed.buffer(), "hello");
    EXPECT_EQ(ed.cursor(), 5u);

    EXPECT_TRUE(ed.handle(key(EditorKey::Enter)));
    EXPECT_TRUE(ed.buffer().empty());
    ASSERT_EQ(ed.history().size(), 1u);
    EXPECT_EQ(ed.history().back(), "hello");
}

TEST(FtxuiEditorTest, BackspaceAndDelete) {
    FtxuiEditor ed;
    ed.type("abcd");
    ed.handle(key(EditorKey::Backspace));
    EXPECT_EQ(ed.buffer(), "abc");
    ed.handle(key(EditorKey::Left));
    ed.handle(key(EditorKey::Delete));
    EXPECT_EQ(ed.buffer(), "ab");
    EXPECT_EQ(ed.cursor(), 2u);
}

TEST(FtxuiEditorTest, HomeEndNavigation) {
    FtxuiEditor ed;
    ed.type("hello world");
    ed.handle(key(EditorKey::Home));
    EXPECT_EQ(ed.cursor(), 0u);
    ed.handle(key(EditorKey::End));
    EXPECT_EQ(ed.cursor(), 11u);
}

TEST(FtxuiEditorTest, HistoryUpDown) {
    FtxuiEditor ed;
    ed.type("first");
    ed.handle(key(EditorKey::Enter));
    ed.type("second");
    ed.handle(key(EditorKey::Enter));

    // Start typing something new, then browse history.
    ed.type("draft");
    ed.handle(key(EditorKey::Up));
    EXPECT_EQ(ed.buffer(), "second");
    ed.handle(key(EditorKey::Up));
    EXPECT_EQ(ed.buffer(), "first");
    ed.handle(key(EditorKey::Down));
    EXPECT_EQ(ed.buffer(), "second");
    ed.handle(key(EditorKey::Down));
    // Back to draft.
    EXPECT_EQ(ed.buffer(), "draft");
}

TEST(FtxuiEditorTest, CtrlCClearsBuffer) {
    FtxuiEditor ed;
    ed.type("will be discarded");
    ed.handle(key(EditorKey::CtrlC));
    EXPECT_TRUE(ed.buffer().empty());
    EXPECT_EQ(ed.cursor(), 0u);
    EXPECT_FALSE(ed.eof());
}

TEST(FtxuiEditorTest, CtrlDOnEmptyBufferSetsEof) {
    FtxuiEditor ed;
    ed.handle(key(EditorKey::CtrlD));
    EXPECT_TRUE(ed.eof());
}

TEST(FtxuiEditorTest, AltEnterInsertsNewline) {
    FtxuiEditor ed;
    ed.type("line1");
    ed.handle(key(EditorKey::AltEnter));
    ed.type("line2");
    EXPECT_EQ(ed.buffer(), "line1\nline2");

    auto frame = ed.render();
    EXPECT_EQ(frame.rows.size(), 2u);
    EXPECT_EQ(frame.cursor_row, 1u);
}

TEST(FtxuiEditorTest, CompletionProviderAndTabAccept) {
    FtxuiEditor ed;
    ed.set_completion_provider(
        [](const std::string& buf, std::size_t /*cursor*/) {
            if (buf == "/he") return std::vector<std::string>{"/help", "/hello"};
            return std::vector<std::string>{};
        });
    ed.type("/he");
    auto frame = ed.render();
    EXPECT_EQ(frame.completions.size(), 2u);

    // Tab cycles; first Tab is already at index 0 (recompute).
    ed.handle(key(EditorKey::Tab));   // cycle -> 1
    ed.handle(key(EditorKey::Enter)); // accept selection, not submit
    EXPECT_EQ(ed.buffer(), "/hello");
    // Enter now submits.
    ed.handle(key(EditorKey::Enter));
    EXPECT_EQ(ed.history().back(), "/hello");
}

TEST(FtxuiEditorTest, RenderLinePadsNotEraseToEol) {
    FtxuiEditor ed;
    ed.set_terminal_width(20);
    ed.type("hi");
    auto frame = ed.render();
    // Prompt + "hi" plus spaces to width 20.
    ASSERT_FALSE(frame.rows.empty());
    EXPECT_EQ(frame.rows[0].size(), 20u);
    // No \033[K in output.
    for (const auto& r : frame.rows) {
        EXPECT_EQ(r.find("\033[K"), std::string::npos);
    }
}

TEST(FtxuiEditorTest, HistoryDedupesConsecutive) {
    FtxuiEditor ed;
    ed.type("same");
    ed.handle(key(EditorKey::Enter));
    ed.type("same");
    ed.handle(key(EditorKey::Enter));
    EXPECT_EQ(ed.history().size(), 1u);
}

TEST(FtxuiEditorTest, HistoryRespectsLimit) {
    FtxuiEditor ed;
    ed.set_history_limit(3);
    for (int i = 0; i < 5; ++i) {
        ed.type(std::string(1, 'a' + i));
        ed.handle(key(EditorKey::Enter));
    }
    EXPECT_EQ(ed.history().size(), 3u);
    EXPECT_EQ(ed.history().front(), "c");
    EXPECT_EQ(ed.history().back(), "e");
}

TEST(FtxuiEditorTest, EscapeDismissesCompletions) {
    FtxuiEditor ed;
    ed.set_completion_provider(
        [](const std::string&, std::size_t) {
            return std::vector<std::string>{"one", "two"};
        });
    ed.type("x");
    auto f = ed.render();
    EXPECT_EQ(f.completions.size(), 2u);
    ed.handle(key(EditorKey::Escape));
    f = ed.render();
    EXPECT_TRUE(f.completions.empty());
}

}  // namespace
}  // namespace hermes::cli
