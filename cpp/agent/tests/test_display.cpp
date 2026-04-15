// Unit tests for hermes::agent::display — C++17 port of agent/display.py.
#include "hermes/agent/display.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace hermes::agent::display;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class TempCwd : public ::testing::Test {
protected:
    fs::path tmp;
    fs::path saved_cwd;

    void SetUp() override {
        saved_cwd = fs::current_path();
        tmp = fs::temp_directory_path() / ("hermes_display_test_" + std::to_string(::getpid()));
        fs::create_directories(tmp);
        fs::current_path(tmp);
        set_tool_preview_max_len(0);  // unlimited
    }
    void TearDown() override {
        fs::current_path(saved_cwd);
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
};

}  // namespace

TEST(Display, OnelineCollapsesWhitespace) {
    EXPECT_EQ(oneline("  hello\n  world\t!"), "hello world !");
    EXPECT_EQ(oneline(""), "");
    EXPECT_EQ(oneline("   "), "");
    EXPECT_EQ(oneline("a"), "a");
}

TEST(Display, ToolPreviewMaxLenAccessors) {
    set_tool_preview_max_len(50);
    EXPECT_EQ(get_tool_preview_max_len(), 50);
    set_tool_preview_max_len(-3);
    EXPECT_EQ(get_tool_preview_max_len(), 0);
    set_tool_preview_max_len(0);
    EXPECT_EQ(get_tool_preview_max_len(), 0);
}

TEST(Display, BuildToolPreviewTerminal) {
    auto p = build_tool_preview("terminal", json{{"command", "ls -la"}});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "ls -la");
}

TEST(Display, BuildToolPreviewWebSearch) {
    auto p = build_tool_preview("web_search", json{{"query", "site:example.com hello world"}});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "site:example.com hello world");
}

TEST(Display, BuildToolPreviewProcess) {
    auto p = build_tool_preview("process",
        json{{"action", "wait"}, {"session_id", "abc12345678901234567890"},
             {"data", "echo"}, {"timeout", 30}});
    ASSERT_TRUE(p.has_value());
    EXPECT_NE(p->find("wait"), std::string::npos);
    EXPECT_NE(p->find("abc1234567890123"), std::string::npos);
    EXPECT_NE(p->find("30s"), std::string::npos);
}

TEST(Display, BuildToolPreviewTodoStates) {
    // Empty args follows Python: returns None (the model never calls todo with
    // truly-empty args; nullable todos appears in a populated dict instead).
    EXPECT_FALSE(build_tool_preview("todo", json::object()).has_value());
    auto reading = build_tool_preview("todo", json{{"todos", nullptr}});
    EXPECT_EQ(reading.value_or(""), "reading task list");
    auto plan = build_tool_preview("todo", json{{"todos", json::array({"a", "b"})}});
    EXPECT_EQ(plan.value_or(""), "planning 2 task(s)");
    auto upd = build_tool_preview("todo",
        json{{"todos", json::array({"a"})}, {"merge", true}});
    EXPECT_EQ(upd.value_or(""), "updating 1 task(s)");
}

TEST(Display, BuildToolPreviewMemory) {
    auto add = build_tool_preview("memory",
        json{{"action", "add"}, {"target", "facts"}, {"content", "the sky is blue"}});
    ASSERT_TRUE(add.has_value());
    EXPECT_EQ(*add, "+facts: \"the sky is blue\"");

    auto rep = build_tool_preview("memory",
        json{{"action", "replace"}, {"target", "facts"}, {"old_text", "old"}});
    ASSERT_TRUE(rep.has_value());
    EXPECT_EQ(*rep, "~facts: \"old\"");
}

TEST(Display, BuildToolPreviewRlVariants) {
    // RL helpers expect at least one populated key — empty args returns
    // nullopt to match Python's `if not args` early-out.
    EXPECT_EQ(build_tool_preview("rl_list_environments", json{{"_", "x"}}).value_or(""), "listing envs");
    EXPECT_EQ(build_tool_preview("rl_select_environment", json{{"name", "tb2"}}).value_or(""), "tb2");
    EXPECT_EQ(build_tool_preview("rl_test_inference", json{{"num_steps", 7}}).value_or(""), "7 steps");
    // When num_steps key is absent but at least one other key exists, default to 3.
    EXPECT_EQ(build_tool_preview("rl_test_inference", json{{"_", "x"}}).value_or(""), "3 steps");
}

TEST(Display, BuildToolPreviewFallbackKey) {
    auto p = build_tool_preview("custom_unknown_tool", json{{"query", "fallback hit"}});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(*p, "fallback hit");
}

TEST(Display, BuildToolPreviewMaxLenTruncates) {
    set_tool_preview_max_len(10);
    auto p = build_tool_preview("terminal", json{{"command", "this is a very long command line"}});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->size(), 10u);
    EXPECT_EQ(p->substr(p->size() - 3), "...");
    set_tool_preview_max_len(0);
}

TEST(Display, BuildToolPreviewEmptyArgs) {
    EXPECT_FALSE(build_tool_preview("terminal", json::object()).has_value());
    EXPECT_FALSE(build_tool_preview("noop_tool", json::object()).has_value());
}

TEST(Display, DetectToolFailureTerminal) {
    auto ok = detect_tool_failure("terminal", std::string{R"({"exit_code":0})"});
    EXPECT_FALSE(ok.is_failure);

    auto bad = detect_tool_failure("terminal", std::string{R"({"exit_code":2})"});
    EXPECT_TRUE(bad.is_failure);
    EXPECT_EQ(bad.suffix, " [exit 2]");
}

TEST(Display, DetectToolFailureMemoryFull) {
    auto info = detect_tool_failure(
        "memory",
        std::string{R"({"success":false,"error":"would exceed the limit of 200"})"});
    EXPECT_TRUE(info.is_failure);
    EXPECT_EQ(info.suffix, " [full]");
}

TEST(Display, DetectToolFailureGenericErrorWord) {
    auto info = detect_tool_failure("read_file",
        std::string{R"({"error":"missing file"})"});
    EXPECT_TRUE(info.is_failure);
    EXPECT_EQ(info.suffix, " [error]");
}

TEST(Display, DetectToolFailureNullResult) {
    auto info = detect_tool_failure("terminal", std::nullopt);
    EXPECT_FALSE(info.is_failure);
}

TEST(Display, GetCuteToolMessageTerminal) {
    auto line = get_cute_tool_message("terminal", json{{"command", "echo hi"}}, 1.5);
    EXPECT_NE(line.find("$"), std::string::npos);
    EXPECT_NE(line.find("echo hi"), std::string::npos);
    EXPECT_NE(line.find("1.5s"), std::string::npos);
}

TEST(Display, GetCuteToolMessageWithFailureSuffix) {
    auto line = get_cute_tool_message("terminal",
        json{{"command", "false"}}, 0.2, std::string{R"({"exit_code":1})"});
    EXPECT_NE(line.find("[exit 1]"), std::string::npos);
}

TEST(Display, GetCuteToolMessageBrowserScrollArrow) {
    auto down = get_cute_tool_message("browser_scroll", json{{"direction", "down"}}, 0.1);
    EXPECT_NE(down.find("↓"), std::string::npos);
    auto up = get_cute_tool_message("browser_scroll", json{{"direction", "up"}}, 0.1);
    EXPECT_NE(up.find("↑"), std::string::npos);
}

TEST(Display, GetCuteToolMessageDelegateMultiTask) {
    auto line = get_cute_tool_message("delegate_task",
        json{{"tasks", json::array({"a", "b", "c"})}}, 4.0);
    EXPECT_NE(line.find("3 parallel tasks"), std::string::npos);
}

TEST(Display, ResultSucceededBehaviour) {
    EXPECT_FALSE(result_succeeded(std::nullopt));
    EXPECT_FALSE(result_succeeded(std::string{}));
    EXPECT_FALSE(result_succeeded(std::string{"not json"}));
    EXPECT_FALSE(result_succeeded(std::string{R"({"error":"boom"})"}));
    EXPECT_TRUE(result_succeeded(std::string{R"({"ok":true})"}));
    EXPECT_TRUE(result_succeeded(std::string{R"({"success":true})"}));
    EXPECT_FALSE(result_succeeded(std::string{R"({"success":false})"}));
}

TEST_F(TempCwd, CaptureLocalEditSnapshotWriteFile) {
    auto file = tmp / "x.txt";
    {
        std::ofstream out(file);
        out << "before\n";
    }
    auto snap = capture_local_edit_snapshot("write_file", json{{"path", file.string()}});
    ASSERT_TRUE(snap.has_value());
    ASSERT_EQ(snap->paths.size(), 1u);
    auto it = snap->before.find(file.string());
    ASSERT_NE(it, snap->before.end());
    ASSERT_TRUE(it->second.has_value());
    EXPECT_EQ(*it->second, "before\n");
}

TEST_F(TempCwd, CaptureLocalEditSnapshotMissingFileNullopt) {
    auto file = tmp / "doesnotexist.txt";
    auto snap = capture_local_edit_snapshot("write_file", json{{"path", file.string()}});
    ASSERT_TRUE(snap.has_value());
    auto it = snap->before.find(file.string());
    ASSERT_NE(it, snap->before.end());
    EXPECT_FALSE(it->second.has_value());
}

TEST_F(TempCwd, ResolveLocalEditPathsUnknownTool) {
    auto p = resolve_local_edit_paths("read_file", json{{"path", "x"}});
    EXPECT_TRUE(p.empty());
}

TEST_F(TempCwd, DiffFromSnapshotProducesUnifiedDiff) {
    auto file = tmp / "y.txt";
    {
        std::ofstream out(file);
        out << "alpha\nbeta\n";
    }
    auto snap = capture_local_edit_snapshot("write_file", json{{"path", file.string()}});
    ASSERT_TRUE(snap.has_value());
    {
        std::ofstream out(file);
        out << "alpha\nGAMMA\n";
    }
    auto diff = diff_from_snapshot(snap);
    ASSERT_TRUE(diff.has_value());
    EXPECT_NE(diff->find("--- a/"), std::string::npos);
    EXPECT_NE(diff->find("+++ b/"), std::string::npos);
    EXPECT_NE(diff->find("-beta"), std::string::npos);
    EXPECT_NE(diff->find("+GAMMA"), std::string::npos);
}

TEST_F(TempCwd, DiffFromSnapshotIdenticalReturnsNullopt) {
    auto file = tmp / "z.txt";
    {
        std::ofstream out(file);
        out << "same\n";
    }
    auto snap = capture_local_edit_snapshot("write_file", json{{"path", file.string()}});
    ASSERT_TRUE(snap.has_value());
    EXPECT_FALSE(diff_from_snapshot(snap).has_value());
}

TEST(Display, ExtractEditDiffPatchUsesResultDiff) {
    json result = {{"diff", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n"}};
    auto out = extract_edit_diff("patch", result.dump());
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("--- a/x"), std::string::npos);
}

TEST(Display, ExtractEditDiffNonEditTool) {
    EXPECT_FALSE(extract_edit_diff("read_file", std::string{R"({"ok":true})"}).has_value());
}

TEST(Display, SplitUnifiedDiffSections) {
    std::string diff =
        "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n"
        "--- a/y\n+++ b/y\n@@ -1 +1 @@\n-c\n+d\n";
    auto sections = split_unified_diff_sections(diff);
    ASSERT_EQ(sections.size(), 2u);
    EXPECT_NE(sections[0].find("--- a/x"), std::string::npos);
    EXPECT_NE(sections[1].find("--- a/y"), std::string::npos);
}

TEST(Display, RenderInlineUnifiedDiffPaintsLines) {
    std::string diff = "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n c\n";
    auto rendered = render_inline_unified_diff(diff);
    ASSERT_FALSE(rendered.empty());
    // Header → "a/x → b/x"
    EXPECT_NE(rendered[0].find("a/x"), std::string::npos);
    EXPECT_NE(rendered[0].find("→"), std::string::npos);
    EXPECT_NE(rendered[0].find(kAnsiReset), std::string::npos);
}

TEST(Display, SummarizeRenderedDiffSectionsTruncates) {
    // 3 sections, max_files=1 → omit summary line emitted.
    std::string diff =
        "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n"
        "--- a/y\n+++ b/y\n@@ -1 +1 @@\n-c\n+d\n"
        "--- a/z\n+++ b/z\n@@ -1 +1 @@\n-e\n+f\n";
    auto rendered = summarize_rendered_diff_sections(diff, /*max_files=*/1, /*max_lines=*/100);
    ASSERT_FALSE(rendered.empty());
    EXPECT_NE(rendered.back().find("omitted"), std::string::npos);
    EXPECT_NE(rendered.back().find("additional file(s)/section(s)"), std::string::npos);
}

TEST(Display, FormatContextPressureCli) {
    auto line = format_context_pressure(0.5, 100'000, 0.85, true);
    EXPECT_NE(line.find("50% to compaction"), std::string::npos);
    EXPECT_NE(line.find("100k threshold"), std::string::npos);
    EXPECT_NE(line.find("85%"), std::string::npos);
    EXPECT_NE(line.find("compaction approaching"), std::string::npos);
}

TEST(Display, FormatContextPressureCliCompressionDisabled) {
    auto line = format_context_pressure(1.5, 250, 0.5, false);
    EXPECT_NE(line.find("100% to compaction"), std::string::npos);  // capped
    EXPECT_NE(line.find("250 threshold"), std::string::npos);
    EXPECT_NE(line.find("no auto-compaction"), std::string::npos);
}

TEST(Display, FormatContextPressureGateway) {
    auto plain = format_context_pressure_gateway(0.25, 0.85, true);
    EXPECT_EQ(plain.find("\033"), std::string::npos);  // no ANSI
    EXPECT_NE(plain.find("25%"), std::string::npos);
    EXPECT_NE(plain.find("85%"), std::string::npos);

    auto disabled = format_context_pressure_gateway(0.99, 0.85, false);
    EXPECT_NE(disabled.find("Auto-compaction is disabled"), std::string::npos);
}

TEST(Display, KawaiiSpinnerDataTables) {
    EXPECT_FALSE(kawaii_waiting_faces().empty());
    EXPECT_FALSE(kawaii_thinking_faces().empty());
    EXPECT_FALSE(thinking_verbs().empty());
    EXPECT_FALSE(spinner_frames("dots").empty());
    EXPECT_FALSE(spinner_frames("nonexistent_kind").empty());  // falls back to dots
    EXPECT_EQ(spinner_frames("nonexistent_kind"), spinner_frames("dots"));

    auto kinds = spinner_kinds();
    EXPECT_NE(std::find(kinds.begin(), kinds.end(), "dots"), kinds.end());
    EXPECT_NE(std::find(kinds.begin(), kinds.end(), "moon"), kinds.end());
}
