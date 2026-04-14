// Coverage for the expanded `file_operations` surface — encoding
// detection, BOM handling, image metadata, notebook round-trip, fuzzy
// unified-diff application, write-deny list, symlink cycle guard, MIME
// sniffing, and search budget / mode behaviour.
#include "hermes/tools/file_operations.hpp"
#include "hermes/tools/file_tools.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace hermes::tools;

namespace {

class FileOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_file_tools();
        auto test_name =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        tmp_ = fs::temp_directory_path() /
               (std::string("hermes_file_ops_") + test_name + "_" +
                std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        fs::create_directories(tmp_);
    }
    void TearDown() override {
        ToolRegistry::instance().clear();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }
    void write_tmp(const std::string& name, const std::string& content) {
        std::ofstream out(tmp_ / name, std::ios::binary);
        out.write(content.data(), content.size());
    }
    std::string dispatch(const std::string& name, const json& args) {
        ToolContext ctx;
        ctx.cwd = tmp_.string();
        return ToolRegistry::instance().dispatch(name, args, ctx);
    }
    fs::path tmp_;
};

// -- encoding / BOM -------------------------------------------------------

TEST_F(FileOpsTest, DetectsUtf8Bom) {
    std::string s = "\xEF\xBB\xBFhello";
    EXPECT_EQ(detect_encoding(s), "utf-8-bom");
    EXPECT_EQ(strip_bom(s), 3u);
    EXPECT_EQ(s, "hello");
}

TEST_F(FileOpsTest, DetectsUtf16Boms) {
    EXPECT_EQ(detect_encoding(std::string("\xFF\xFE", 2)), "utf-16le");
    EXPECT_EQ(detect_encoding(std::string("\xFE\xFF", 2)), "utf-16be");
}

TEST_F(FileOpsTest, DetectsUtf32Boms) {
    EXPECT_EQ(detect_encoding(std::string("\xFF\xFE\x00\x00", 4)), "utf-32le");
    EXPECT_EQ(detect_encoding(std::string("\x00\x00\xFE\xFF", 4)), "utf-32be");
}

TEST_F(FileOpsTest, NormalisesCrLf) {
    EXPECT_EQ(normalise_newlines("a\r\nb\r\nc"), "a\nb\nc");
    EXPECT_EQ(normalise_newlines("a\rb"), "a\nb");
    EXPECT_EQ(normalise_newlines("a\nb"), "a\nb");
}

// -- MIME -----------------------------------------------------------------

TEST_F(FileOpsTest, MimeTypeFromExtension) {
    EXPECT_EQ(detect_mime_type("foo.py"), "text/x-python");
    EXPECT_EQ(detect_mime_type("foo.json"), "application/json");
    EXPECT_EQ(detect_mime_type("foo.png"), "image/png");
    EXPECT_EQ(detect_mime_type("unknown.xyz"), "application/octet-stream");
}

// -- image magic ----------------------------------------------------------

TEST_F(FileOpsTest, ParsesPngMagic) {
    // 1x1 red PNG.
    unsigned char png[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
        0, 0, 0, 2, 0, 0, 0, 3,  // 2x3
        8, 2, 0, 0, 0};
    auto info = parse_image_info(
        std::string(reinterpret_cast<char*>(png), sizeof(png)));
    EXPECT_EQ(info.format, "png");
    EXPECT_EQ(info.width, 2);
    EXPECT_EQ(info.height, 3);
}

TEST_F(FileOpsTest, ParsesGifMagic) {
    unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a',
                            0x10, 0x00, 0x20, 0x00};  // 16x32
    auto info = parse_image_info(
        std::string(reinterpret_cast<char*>(gif), sizeof(gif)));
    EXPECT_EQ(info.format, "gif");
    EXPECT_EQ(info.width, 16);
    EXPECT_EQ(info.height, 32);
}

TEST_F(FileOpsTest, ParsesBmpMagic) {
    unsigned char bmp[40] = {0};
    bmp[0] = 'B'; bmp[1] = 'M';
    // width at offset 18 LE = 40
    bmp[18] = 40;
    // height at offset 22 LE = 25
    bmp[22] = 25;
    auto info = parse_image_info(
        std::string(reinterpret_cast<char*>(bmp), sizeof(bmp)));
    EXPECT_EQ(info.format, "bmp");
    EXPECT_EQ(info.width, 40);
    EXPECT_EQ(info.height, 25);
}

TEST_F(FileOpsTest, NonImageHasEmptyFormat) {
    auto info = parse_image_info("hello world text");
    EXPECT_TRUE(info.format.empty());
}

// -- base64 ---------------------------------------------------------------

TEST_F(FileOpsTest, Base64EncodesKnownValues) {
    EXPECT_EQ(base64_encode("Man"), "TWFu");
    EXPECT_EQ(base64_encode("Ma"), "TWE=");
    EXPECT_EQ(base64_encode("M"), "TQ==");
    EXPECT_EQ(base64_encode(""), "");
}

// -- write-deny list ------------------------------------------------------

TEST_F(FileOpsTest, WriteDeniedForEtcPasswd) {
    EXPECT_TRUE(is_write_denied("/etc/passwd"));
    EXPECT_TRUE(is_write_denied("/etc/shadow"));
}

TEST_F(FileOpsTest, WriteAllowedForTempFile) {
    unsetenv("HERMES_WRITE_SAFE_ROOT");
    EXPECT_FALSE(is_write_denied(tmp_ / "ok.txt"));
}

TEST_F(FileOpsTest, SafeRootSandboxBlocksOutsideWrites) {
    setenv("HERMES_WRITE_SAFE_ROOT", tmp_.c_str(), 1);
    EXPECT_FALSE(is_write_denied(tmp_ / "inside.txt"));
    EXPECT_TRUE(is_write_denied("/tmp/outside-" +
                                 std::to_string(std::rand()) + ".txt"));
    unsetenv("HERMES_WRITE_SAFE_ROOT");
}

// -- notebook -------------------------------------------------------------

TEST_F(FileOpsTest, ParsesNotebookCells) {
    std::string nb = R"({
        "cells": [
            {"cell_type": "markdown", "source": ["# Title\n", "text"]},
            {"cell_type": "code", "source": "x = 1\n", "execution_count": 3,
             "outputs": [{"output_type": "stream", "name": "stdout",
                          "text": ["hi\n"]}]}
        ],
        "metadata": {},
        "nbformat": 4, "nbformat_minor": 5
    })";
    auto cells = parse_notebook(nb);
    ASSERT_EQ(cells.size(), 2u);
    EXPECT_EQ(cells[0].cell_type, "markdown");
    EXPECT_EQ(cells[0].source, "# Title\ntext");
    EXPECT_EQ(cells[1].cell_type, "code");
    EXPECT_EQ(cells[1].execution_count, 3);
    ASSERT_EQ(cells[1].outputs.size(), 1u);
    EXPECT_EQ(cells[1].outputs[0], "hi\n");
}

TEST_F(FileOpsTest, NotebookRoundTripPreservesStructure) {
    std::string nb = R"({"cells":[],"metadata":{"kernelspec":{"name":"python3"}},"nbformat":4,"nbformat_minor":5})";
    std::vector<NotebookCell> cells(1);
    cells[0].cell_type = "code";
    cells[0].source = "print('hi')\n";
    cells[0].execution_count = 1;
    auto out = edit_notebook(nb, cells);
    auto j = json::parse(out);
    EXPECT_EQ(j["metadata"]["kernelspec"]["name"], "python3");
    ASSERT_TRUE(j["cells"].is_array());
    EXPECT_EQ(j["cells"][0]["cell_type"], "code");
}

TEST_F(FileOpsTest, RendersNotebookToText) {
    std::vector<NotebookCell> cells(2);
    cells[0].cell_type = "markdown";
    cells[0].source = "# Hello";
    cells[1].cell_type = "code";
    cells[1].source = "x = 1";
    cells[1].execution_count = 2;
    cells[1].outputs.push_back("value\n");
    auto text = render_notebook(cells);
    EXPECT_NE(text.find("markdown cell 1"), std::string::npos);
    EXPECT_NE(text.find("# Hello"), std::string::npos);
    EXPECT_NE(text.find("code cell 2"), std::string::npos);
    EXPECT_NE(text.find("in[2]"), std::string::npos);
    EXPECT_NE(text.find("value"), std::string::npos);
}

// -- unified-diff fuzz ----------------------------------------------------

TEST_F(FileOpsTest, ApplyUnifiedDiffExactMatch) {
    std::string orig = "a\nb\nc\n";
    std::string diff =
        "--- a/x\n+++ b/x\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n";
    int applied = 0;
    std::string err;
    auto out = apply_unified_diff(orig, diff, applied, err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(out, "a\nB\nc\n");
}

TEST_F(FileOpsTest, ApplyUnifiedDiffDriftsWithinFuzz) {
    // Hunk claims line 1 but the match is at line 3.
    std::string orig = "pre1\npre2\na\nb\nc\n";
    std::string diff =
        "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n";
    int applied = 0;
    std::string err;
    auto out = apply_unified_diff(orig, diff, applied, err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(applied, 1);
    EXPECT_NE(out.find("pre1\npre2\na\nB\nc\n"), std::string::npos);
}

TEST_F(FileOpsTest, ApplyUnifiedDiffRejectsUnknownContext) {
    std::string orig = "aaa\nbbb\nccc\n";
    std::string diff = "@@ -1,3 +1,3 @@\n zzz\n-bbb\n+yyy\n ccc\n";
    int applied = 0;
    std::string err;
    auto out = apply_unified_diff(orig, diff, applied, err);
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(out, orig);
}

TEST_F(FileOpsTest, ApplyUnifiedDiffWhitespaceTolerant) {
    std::string orig = "foo()\n  bar()\nbaz()\n";
    std::string diff =
        "@@ -1,3 +1,3 @@\n foo()\n-bar()\n+BAR()\n baz()\n";
    ApplyOptions opts;
    opts.ignore_whitespace = true;
    int applied = 0;
    std::string err;
    auto out = apply_unified_diff(orig, diff, applied, err, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(applied, 1);
    EXPECT_NE(out.find("BAR()"), std::string::npos);
}

// -- symlink --------------------------------------------------------------

TEST_F(FileOpsTest, SymlinkCycleDetected) {
    fs::create_directory_symlink(tmp_ / "a", tmp_ / "b");
    fs::create_directory_symlink(tmp_ / "b", tmp_ / "a");
    auto r = resolve_symlink_safe(tmp_ / "a");
    EXPECT_TRUE(r.empty());
}

// -- add_line_numbers -----------------------------------------------------

TEST_F(FileOpsTest, AddLineNumbersFormatsSixWideGutter) {
    auto out = add_line_numbers("a\nb", 1);
    EXPECT_NE(out.find("     1|a"), std::string::npos);
    EXPECT_NE(out.find("     2|b"), std::string::npos);
}

TEST_F(FileOpsTest, AddLineNumbersTruncatesLongLines) {
    std::string long_line(kMaxLineLength + 50, 'x');
    auto out = add_line_numbers(long_line, 1);
    EXPECT_NE(out.find("[truncated]"), std::string::npos);
}

// -- read_file tool: image + notebook + binary ----------------------------

TEST_F(FileOpsTest, ReadFileDetectsImage) {
    unsigned char png[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
        0, 0, 0, 4, 0, 0, 0, 5, 8, 2, 0, 0, 0};
    write_tmp("img.png",
              std::string(reinterpret_cast<char*>(png), sizeof(png)));
    auto result = json::parse(dispatch("read_file", {{"path", "img.png"}}));
    ASSERT_TRUE(result.contains("is_image"));
    EXPECT_TRUE(result["is_image"].get<bool>());
    EXPECT_EQ(result["width"], 4);
    EXPECT_EQ(result["height"], 5);
    EXPECT_TRUE(result.contains("base64"));
}

TEST_F(FileOpsTest, ReadFileDetectsNotebook) {
    std::string nb = R"({"cells":[{"cell_type":"markdown","source":"# nb"}],
                        "metadata":{},"nbformat":4,"nbformat_minor":5})";
    write_tmp("n.ipynb", nb);
    auto r = json::parse(dispatch("read_file", {{"path", "n.ipynb"}}));
    ASSERT_TRUE(r.contains("is_notebook"));
    EXPECT_TRUE(r["is_notebook"].get<bool>());
    EXPECT_EQ(r["cell_count"], 1);
    EXPECT_NE(r["content"].get<std::string>().find("# nb"),
              std::string::npos);
}

TEST_F(FileOpsTest, ReadFileRejectsBinary) {
    std::string bin = "\x00\x01\x02\x03binary";
    write_tmp("data.bin", bin);
    auto r = json::parse(dispatch("read_file", {{"path", "data.bin"}}));
    EXPECT_TRUE(r.contains("is_binary"));
    EXPECT_TRUE(r.contains("error"));
}

TEST_F(FileOpsTest, ReadFileReturnsLineNumbersByDefault) {
    write_tmp("t.txt", "alpha\nbeta\ngamma\n");
    auto r = json::parse(dispatch("read_file", {{"path", "t.txt"}}));
    EXPECT_NE(r["content"].get<std::string>().find("     1|alpha"),
              std::string::npos);
    EXPECT_EQ(r["total_lines"], 3);
}

TEST_F(FileOpsTest, ReadFileReportsStatMetadata) {
    write_tmp("s.txt", "hello");
    auto r = json::parse(dispatch("read_file", {{"path", "s.txt"}}));
    EXPECT_TRUE(r.contains("mode"));
    EXPECT_TRUE(r.contains("mtime"));
    EXPECT_EQ(r["size"], 5);
    EXPECT_EQ(r["mime"], "text/plain");
}

TEST_F(FileOpsTest, WriteFileThenPatchFuzzy) {
    write_tmp("p.txt", "first\nsecond\nthird\n");
    // Drop leading blank line to force fuzzy drift.
    std::string diff =
        "@@ -1,3 +1,3 @@\n first\n-second\n+SECOND\n third\n";
    auto res = json::parse(
        dispatch("patch", {{"path", "p.txt"}, {"diff", diff}}));
    EXPECT_TRUE(res["patched"].get<bool>());
    auto rd = json::parse(
        dispatch("read_file", {{"path", "p.txt"}, {"raw", true}}));
    EXPECT_NE(rd["content"].get<std::string>().find("SECOND"),
              std::string::npos);
}

TEST_F(FileOpsTest, SearchRespectsFilesOnlyMode) {
    write_tmp("a.txt", "needle here\n");
    write_tmp("b.txt", "no match\n");
    auto r = json::parse(dispatch(
        "search_files",
        {{"pattern", "needle"}, {"path", "."}, {"mode", "files_only"}}));
    ASSERT_TRUE(r.contains("files"));
    EXPECT_EQ(r["files"].size(), 1u);
    EXPECT_NE(r["files"][0].get<std::string>().find("a.txt"),
              std::string::npos);
}

TEST_F(FileOpsTest, SearchRespectsCountMode) {
    write_tmp("a.txt", "x\nx\nx\n");
    write_tmp("b.txt", "x\n");
    auto r = json::parse(dispatch(
        "search_files",
        {{"pattern", "x"}, {"path", "."}, {"mode", "count"}}));
    ASSERT_TRUE(r.contains("counts"));
    EXPECT_EQ(r["total_count"], 4);
}

TEST_F(FileOpsTest, SearchLiteralEscapesRegexMeta) {
    write_tmp("dot.txt", "a.b\nA_B\n");
    auto r = json::parse(dispatch(
        "search_files",
        {{"pattern", "a.b"}, {"literal", true}, {"path", "."}}));
    // Literal `.` matches only "a.b" not "A_B".
    EXPECT_EQ(r["matches"].size(), 1u);
}

TEST_F(FileOpsTest, SearchSkipsHiddenDirectories) {
    fs::create_directories(tmp_ / ".git");
    write_tmp(".git/secret.txt", "needle");
    write_tmp("visible.txt", "needle");
    auto r = json::parse(dispatch(
        "search_files", {{"pattern", "needle"}, {"path", "."}}));
    EXPECT_EQ(r["matches"].size(), 1u);
}

TEST_F(FileOpsTest, SearchRespectsMaxResultsBudget) {
    for (int i = 0; i < 20; ++i)
        write_tmp("f" + std::to_string(i) + ".txt", "needle\n");
    auto r = json::parse(dispatch(
        "search_files",
        {{"pattern", "needle"}, {"path", "."}, {"max_results", 5}}));
    EXPECT_LE(r["matches"].size(), 5u);
}

TEST_F(FileOpsTest, ExpandUserPreservesUnknownUser) {
    EXPECT_EQ(expand_user("~nosuchuser/path"), "~nosuchuser/path");
}

}  // namespace
