// Tests for the trajectory + manual compression feedback helpers.
#include "hermes/agent/trajectory.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using hermes::agent::trajectory::convert_scratchpad_to_think;
using hermes::agent::trajectory::has_incomplete_scratchpad;
using hermes::agent::trajectory::save_trajectory;
using hermes::agent::compression_feedback::summarize_manual_compression;

TEST(TrajectoryHelpers, ConvertScratchpadTags) {
    EXPECT_EQ(convert_scratchpad_to_think("no tags here"), "no tags here");
    EXPECT_EQ(convert_scratchpad_to_think(
                  "pre <REASONING_SCRATCHPAD>x</REASONING_SCRATCHPAD> post"),
              "pre <think>x</think> post");
    // Multiple pairs.
    EXPECT_EQ(convert_scratchpad_to_think(
                  "<REASONING_SCRATCHPAD>a</REASONING_SCRATCHPAD>"
                  "<REASONING_SCRATCHPAD>b</REASONING_SCRATCHPAD>"),
              "<think>a</think><think>b</think>");
}

TEST(TrajectoryHelpers, IncompleteScratchpadDetected) {
    EXPECT_FALSE(has_incomplete_scratchpad(""));
    EXPECT_FALSE(has_incomplete_scratchpad("plain text"));
    EXPECT_TRUE(has_incomplete_scratchpad("<REASONING_SCRATCHPAD>oops"));
    EXPECT_FALSE(has_incomplete_scratchpad(
        "<REASONING_SCRATCHPAD>ok</REASONING_SCRATCHPAD>"));
}

TEST(TrajectoryHelpers, SaveTrajectoryAppendsJsonl) {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() /
                   ("hermes_traj_" + std::to_string(::getpid()) + ".jsonl");
    fs::remove(tmp);
    nlohmann::json convs = nlohmann::json::array({
        {{"from", "user"}, {"value", "hi"}},
        {{"from", "assistant"}, {"value", "ok"}},
    });
    EXPECT_TRUE(save_trajectory(convs, "gpt-test", true, tmp.string()));
    EXPECT_TRUE(save_trajectory(convs, "gpt-test", false, tmp.string()));

    std::ifstream in(tmp);
    std::string line1, line2, extra;
    ASSERT_TRUE(std::getline(in, line1));
    ASSERT_TRUE(std::getline(in, line2));
    EXPECT_FALSE(std::getline(in, extra));

    auto j1 = nlohmann::json::parse(line1);
    auto j2 = nlohmann::json::parse(line2);
    EXPECT_EQ(j1["model"], "gpt-test");
    EXPECT_TRUE(j1["completed"].get<bool>());
    EXPECT_FALSE(j2["completed"].get<bool>());

    fs::remove(tmp);
}

TEST(TrajectoryHelpers, SaveDefaultFilenameCompletion) {
    namespace fs = std::filesystem;
    fs::path prev_cwd = fs::current_path();
    fs::path work = fs::temp_directory_path() /
                    ("hermes_traj_cwd_" + std::to_string(::getpid()));
    fs::create_directories(work);
    fs::current_path(work);

    nlohmann::json convs = nlohmann::json::array();
    EXPECT_TRUE(save_trajectory(convs, "m", true));
    EXPECT_TRUE(fs::exists(work / "trajectory_samples.jsonl"));
    EXPECT_TRUE(save_trajectory(convs, "m", false));
    EXPECT_TRUE(fs::exists(work / "failed_trajectories.jsonl"));

    fs::current_path(prev_cwd);
    fs::remove_all(work);
}

TEST(ManualCompressionFeedback, NoopUnchangedTokens) {
    std::vector<nlohmann::json> before = {nlohmann::json{{"role", "user"}}};
    auto s = summarize_manual_compression(before, before, 1000, 1000);
    EXPECT_TRUE(s.noop);
    EXPECT_NE(s.headline.find("No changes"), std::string::npos);
    EXPECT_NE(s.token_line.find("unchanged"), std::string::npos);
    EXPECT_TRUE(s.note.empty());
}

TEST(ManualCompressionFeedback, NoopDifferentTokens) {
    std::vector<nlohmann::json> before = {nlohmann::json{{"role", "user"}}};
    auto s = summarize_manual_compression(before, before, 1000, 900);
    EXPECT_TRUE(s.noop);
    EXPECT_NE(s.token_line.find("→"), std::string::npos);
}

TEST(ManualCompressionFeedback, RealCompressionHeadline) {
    std::vector<nlohmann::json> before = {
        nlohmann::json{{"role", "user"}},
        nlohmann::json{{"role", "user"}},
        nlohmann::json{{"role", "user"}},
    };
    std::vector<nlohmann::json> after = {nlohmann::json{{"role", "user"}}};
    auto s = summarize_manual_compression(before, after, 3000, 1500);
    EXPECT_FALSE(s.noop);
    EXPECT_NE(s.headline.find("3 → 1"), std::string::npos);
    EXPECT_TRUE(s.note.empty());  // tokens went down, no denser-summary note
}

TEST(ManualCompressionFeedback, DenserSummaryNote) {
    std::vector<nlohmann::json> before = {
        nlohmann::json{{"role", "user"}},
        nlohmann::json{{"role", "user"}},
        nlohmann::json{{"role", "user"}},
    };
    std::vector<nlohmann::json> after = {nlohmann::json{{"role", "system"}}};
    auto s = summarize_manual_compression(before, after, 1000, 3000);
    EXPECT_FALSE(s.noop);
    EXPECT_FALSE(s.note.empty());
}

TEST(ManualCompressionFeedback, FormattedWithCommas) {
    std::vector<nlohmann::json> before = {nlohmann::json{{"role", "user"}}};
    std::vector<nlohmann::json> after = {nlohmann::json{{"role", "system"}}};
    auto s = summarize_manual_compression(before, after, 1234567, 42);
    EXPECT_NE(s.token_line.find("1,234,567"), std::string::npos);
}
