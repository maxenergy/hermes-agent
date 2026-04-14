// Tests for hermes::environments::tool_context — upload/download plan
// builders and terminal-result parsing.
#include "hermes/environments/tool_context.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace tc = hermes::environments::tool_context;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// base64 round-trip
// ---------------------------------------------------------------------------

TEST(ToolContextBase64, RoundTripSimple) {
    std::string in = "hello, world\n";
    auto enc = tc::base64_encode(in);
    auto dec = tc::base64_decode(enc);
    ASSERT_TRUE(dec.has_value());
    std::string out(dec->begin(), dec->end());
    EXPECT_EQ(out, in);
}

TEST(ToolContextBase64, RoundTripBinary) {
    std::vector<std::uint8_t> raw;
    for (int i = 0; i < 256; ++i) raw.push_back(static_cast<std::uint8_t>(i));
    auto enc = tc::base64_encode(raw.data(), raw.size());
    EXPECT_EQ(enc.size() % 4, 0u);
    auto dec = tc::base64_decode(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, raw);
}

TEST(ToolContextBase64, PaddingEncoded) {
    // 1 byte -> "XX==", 2 bytes -> "XXX=".
    EXPECT_EQ(tc::base64_encode(std::string_view("A", 1)), "QQ==");
    EXPECT_EQ(tc::base64_encode(std::string_view("AB", 2)), "QUI=");
    EXPECT_EQ(tc::base64_encode(std::string_view("ABC", 3)), "QUJD");
}

TEST(ToolContextBase64, DecodeRejectsBadInput) {
    // Bad length.
    EXPECT_FALSE(tc::base64_decode("AAA").has_value());
    // Illegal character.
    EXPECT_FALSE(tc::base64_decode("????").has_value());
}

TEST(ToolContextBase64, DecodeStripsWhitespace) {
    auto dec = tc::base64_decode("QU JD\n  ");
    ASSERT_TRUE(dec.has_value());
    std::string s(dec->begin(), dec->end());
    EXPECT_EQ(s, "ABC");
}

// ---------------------------------------------------------------------------
// shell quoting
// ---------------------------------------------------------------------------

TEST(ToolContextQuote, SimpleWrapsInSingleQuotes) {
    EXPECT_EQ(tc::shell_single_quote("hello"), "'hello'");
}

TEST(ToolContextQuote, EscapesEmbeddedSingleQuote) {
    EXPECT_EQ(tc::shell_single_quote("it's"), "'it'\\''s'");
}

TEST(ToolContextQuote, EmptyStringYieldsEmptyQuotes) {
    EXPECT_EQ(tc::shell_single_quote(""), "''");
}

// ---------------------------------------------------------------------------
// Upload plan — single command
// ---------------------------------------------------------------------------

TEST(ToolContextUpload, SmallPayloadSingleCommand) {
    std::string payload = "abc";
    auto plan = tc::build_upload_plan(payload, "/work/out.txt");
    EXPECT_FALSE(plan.chunked);
    ASSERT_EQ(plan.commands.size(), 1u);
    EXPECT_NE(plan.commands[0].find("base64 -d > /work/out.txt"),
              std::string::npos);
    // Encoded size matches base64_encode of "abc" -> "YWJj".
    EXPECT_EQ(plan.encoded_size, 4u);
}

TEST(ToolContextUpload, LargePayloadChunksStagedFile) {
    // 3 chunks at kChunkSize = 60_000 -> ~180KB of base64 output.
    std::string big(200 * 1024, 'x');
    auto plan = tc::build_upload_plan(big, "/w/big.bin");
    EXPECT_TRUE(plan.chunked);
    // Expect: truncate + N chunks + finalize.
    ASSERT_GE(plan.commands.size(), 3u);
    EXPECT_EQ(plan.commands.front(), ": > /tmp/_hermes_upload.b64");
    EXPECT_NE(plan.commands.back().find("base64 -d /tmp/_hermes_upload.b64"),
              std::string::npos);
    EXPECT_NE(plan.commands.back().find("/w/big.bin"), std::string::npos);
    // Each middle command should append via `>>`.
    for (std::size_t i = 1; i + 1 < plan.commands.size(); ++i) {
        EXPECT_NE(plan.commands[i].find(">>"), std::string::npos);
    }
}

TEST(ToolContextUpload, ChunkSizeConfigurable) {
    std::string mid(10, 'A');
    auto plan = tc::build_upload_plan(mid, "/w/x", /*chunk_size=*/4);
    EXPECT_TRUE(plan.chunked);
    // Encoded size = 16 -> chunks of 4 = 4 chunks, plus truncate + finalize.
    EXPECT_EQ(plan.commands.size(), 6u);
}

TEST(ToolContextUpload, ParentMkdirHelper) {
    EXPECT_EQ(tc::remote_mkdir_parent("foo"), std::nullopt);
    EXPECT_EQ(tc::remote_mkdir_parent("/foo"), std::nullopt);
    ASSERT_TRUE(tc::remote_mkdir_parent("/work/out/a.bin").has_value());
    EXPECT_EQ(*tc::remote_mkdir_parent("/work/out/a.bin"), "/work/out");
    EXPECT_EQ(*tc::remote_mkdir_parent("a/b/c"), "a/b");
}

// ---------------------------------------------------------------------------
// Download command + parse
// ---------------------------------------------------------------------------

TEST(ToolContextDownload, BuildCommand) {
    auto cmd = tc::build_download_command("/w/f.txt");
    EXPECT_EQ(cmd, "base64 /w/f.txt 2>/dev/null");
}

TEST(ToolContextDownload, DecodeSuccess) {
    auto enc = tc::base64_encode("payload!");
    auto r = tc::decode_download_output("  " + enc + "\n\n");
    EXPECT_TRUE(r.success);
    std::string s(r.bytes.begin(), r.bytes.end());
    EXPECT_EQ(s, "payload!");
}

TEST(ToolContextDownload, DecodeEmptyIsFailure) {
    auto r = tc::decode_download_output("   \n");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.error.find("empty"), std::string::npos);
}

TEST(ToolContextDownload, DecodeInvalidIsFailure) {
    auto r = tc::decode_download_output("!!!!");
    EXPECT_FALSE(r.success);
}

// ---------------------------------------------------------------------------
// find output parsing + relative paths
// ---------------------------------------------------------------------------

TEST(ToolContextFind, BuildFindCommand) {
    auto cmd = tc::build_remote_find_command("/w");
    EXPECT_EQ(cmd, "find /w -type f 2>/dev/null");
}

TEST(ToolContextFind, ParseFindOutput) {
    auto lines = tc::parse_find_output("a\n\n   b\nc   \n");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
    EXPECT_EQ(lines[2], "c");
}

TEST(ToolContextFind, ParseFindHandlesTrailingNoNewline) {
    auto lines = tc::parse_find_output("only");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "only");
}

TEST(ToolContextFind, RelativePath) {
    EXPECT_EQ(tc::relative_remote_path("/w/a/b.txt", "/w"), "a/b.txt");
    EXPECT_EQ(tc::relative_remote_path("/w/a/b.txt", "/w/"), "a/b.txt");
    // Escapes root -> basename fallback.
    EXPECT_EQ(tc::relative_remote_path("/elsewhere/z", "/w"), "z");
}

// ---------------------------------------------------------------------------
// Terminal-result parsing
// ---------------------------------------------------------------------------

TEST(ToolContextTerminal, ParsesExitAndOutput) {
    auto r = tc::parse_terminal_result(R"({"exit_code": 0, "output": "hello"})");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "hello");
}

TEST(ToolContextTerminal, FallbackForNonJson) {
    auto r = tc::parse_terminal_result("not json");
    EXPECT_EQ(r.exit_code, -1);
    EXPECT_EQ(r.output, "not json");
}

TEST(ToolContextTerminal, MissingOutputFieldReturnsEmpty) {
    auto r = tc::parse_terminal_result(R"({"exit_code": 42})");
    EXPECT_EQ(r.exit_code, 42);
    EXPECT_EQ(r.output, "");
}

// ---------------------------------------------------------------------------
// Local directory enumeration
// ---------------------------------------------------------------------------

TEST(ToolContextLocal, EnumerateDirectorySortedWithRelative) {
    auto root = fs::temp_directory_path() /
                ("hermes-tc-" + std::to_string(::getpid()));
    fs::create_directories(root / "sub");
    {
        std::ofstream(root / "a.txt") << "A";
        std::ofstream(root / "sub" / "b.txt") << "B";
        std::ofstream(root / "sub" / "c.txt") << "C";
    }

    auto entries = tc::enumerate_local_dir(root);
    ASSERT_EQ(entries.size(), 3u);
    // Sorted by absolute path.
    EXPECT_LE(entries[0].absolute, entries[1].absolute);
    EXPECT_LE(entries[1].absolute, entries[2].absolute);
    // Relative paths use forward slashes.
    bool found_nested = false;
    for (const auto& e : entries) {
        if (e.relative == "sub/b.txt" || e.relative == "sub/c.txt") {
            found_nested = true;
            break;
        }
    }
    EXPECT_TRUE(found_nested);
    std::error_code ec;
    fs::remove_all(root, ec);
}
