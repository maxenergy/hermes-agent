#include "hermes/state/trajectory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using hermes::state::TrajectoryRecord;
using hermes::state::TrajectoryWriter;

namespace {
fs::path make_tmpfile() {
    auto base = fs::temp_directory_path() /
                ("hermes_traj_tests_" + std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
    fs::create_directories(base);
    return base / "trajectory.jsonl";
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}
}  // namespace

TEST(TrajectoryTest, WriteAndReadBackJsonl) {
    auto path = make_tmpfile();
    TrajectoryWriter w(path);
    TrajectoryRecord rec;
    rec.timestamp = "2026-04-12T10:00:00Z";
    rec.model = "qwen3-coder";
    rec.messages = nlohmann::json::array(
        {nlohmann::json{{"role", "user"}, {"content", "hi"}}});
    rec.completed = true;
    w.write(rec);

    auto lines = read_lines(path);
    ASSERT_EQ(lines.size(), 1u);
    auto parsed = nlohmann::json::parse(lines[0]);
    EXPECT_EQ(parsed["model"], "qwen3-coder");
    EXPECT_EQ(parsed["completed"], true);
    EXPECT_EQ(parsed["timestamp"], "2026-04-12T10:00:00Z");
    EXPECT_EQ(parsed["conversations"][0]["role"], "user");

    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
}

TEST(TrajectoryTest, AppendModeMultipleWrites) {
    auto path = make_tmpfile();
    TrajectoryWriter w(path);
    for (int i = 0; i < 3; ++i) {
        TrajectoryRecord rec;
        rec.timestamp = "2026-04-12T10:00:0" + std::to_string(i) + "Z";
        rec.model = "m";
        rec.messages = nlohmann::json::array();
        rec.completed = false;
        w.write(rec);
    }
    auto lines = read_lines(path);
    EXPECT_EQ(lines.size(), 3u);

    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
}

TEST(TrajectoryTest, ConvertScratchpadToThinkSimpleCase) {
    auto in = std::string_view("before<REASONING_SCRATCHPAD>thoughts"
                               "</REASONING_SCRATCHPAD>after");
    auto out = TrajectoryWriter::convert_scratchpad_to_think(in);
    EXPECT_EQ(out, "before<think>thoughts</think>after");
}

TEST(TrajectoryTest, ConvertScratchpadNoOpWhenAbsent) {
    auto in = std::string_view("plain text");
    EXPECT_EQ(TrajectoryWriter::convert_scratchpad_to_think(in), "plain text");
}

TEST(TrajectoryTest, HasIncompleteScratchpadDetectsUnclosed) {
    EXPECT_TRUE(TrajectoryWriter::has_incomplete_scratchpad(
        "stuff <REASONING_SCRATCHPAD> unfinished"));
    EXPECT_FALSE(TrajectoryWriter::has_incomplete_scratchpad(
        "<REASONING_SCRATCHPAD>ok</REASONING_SCRATCHPAD>"));
    EXPECT_FALSE(TrajectoryWriter::has_incomplete_scratchpad(""));
    EXPECT_FALSE(
        TrajectoryWriter::has_incomplete_scratchpad("no tags here"));
}

TEST(TrajectoryTest, ConvertMultipleScratchpads) {
    auto in = std::string_view(
        "<REASONING_SCRATCHPAD>a</REASONING_SCRATCHPAD> mid "
        "<REASONING_SCRATCHPAD>b</REASONING_SCRATCHPAD>");
    auto out = TrajectoryWriter::convert_scratchpad_to_think(in);
    EXPECT_EQ(out, "<think>a</think> mid <think>b</think>");
}

TEST(TrajectoryTest, ConvertIncompleteScratchpadStillRenders) {
    auto in = std::string_view("head<REASONING_SCRATCHPAD>unfinished tail");
    auto out = TrajectoryWriter::convert_scratchpad_to_think(in);
    EXPECT_EQ(out, "head<think>unfinished tail");
}
