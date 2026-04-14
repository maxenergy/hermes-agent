// Tests for hermes::cli::logs_cmd.
#include "hermes/cli/logs_cmd.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace hermes::cli::logs_cmd;
namespace fs = std::filesystem;

TEST(LogsCmd, ParseSince_HoursMinutes) {
    auto one_h = parse_since("1h");
    ASSERT_TRUE(one_h.has_value());
    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
                    now - *one_h).count();
    EXPECT_NEAR(diff, 3600, 5);

    auto thirty_m = parse_since("30m");
    ASSERT_TRUE(thirty_m.has_value());
}

TEST(LogsCmd, ParseSince_Invalid) {
    EXPECT_FALSE(parse_since("garbage").has_value());
    EXPECT_FALSE(parse_since("").has_value());
}

TEST(LogsCmd, ExtractLevel_Info) {
    auto level = extract_level("2026-04-05 12:00:00 INFO hello world");
    ASSERT_TRUE(level.has_value());
    EXPECT_EQ(*level, "INFO");
    auto none = extract_level("plain message");
    EXPECT_FALSE(none.has_value());
}

TEST(LogsCmd, ExtractTimestamp_ParsesLeadingIso) {
    auto ts = extract_timestamp("2026-04-05 12:00:00 INFO foo");
    ASSERT_TRUE(ts.has_value());
    auto none = extract_timestamp("no timestamp here");
    EXPECT_FALSE(none.has_value());
}

TEST(LogsCmd, LinePasses_LevelFiltering) {
    Options opts;
    opts.min_level = "WARNING";
    EXPECT_FALSE(line_passes("2026-04-05 12:00:00 INFO hi", opts,
                             std::nullopt));
    EXPECT_TRUE(line_passes("2026-04-05 12:00:00 ERROR hi", opts,
                            std::nullopt));
    EXPECT_TRUE(line_passes("2026-04-05 12:00:00 WARNING hi", opts,
                            std::nullopt));
}

TEST(LogsCmd, LinePasses_SessionSubstr) {
    Options opts;
    opts.session_substr = "sess_abc";
    EXPECT_TRUE(line_passes("session=sess_abc foo", opts, std::nullopt));
    EXPECT_FALSE(line_passes("session=sess_xyz", opts, std::nullopt));
}

TEST(LogsCmd, ReadLastNLines_SmallFile) {
    auto p = fs::temp_directory_path() / "logs_cmd_test.log";
    {
        std::ofstream f(p);
        for (int i = 0; i < 100; ++i) f << "line " << i << "\n";
    }
    auto last = read_last_n_lines(p, 5);
    EXPECT_EQ(last.size(), 5u);
    EXPECT_EQ(last.back(), "line 99");
    EXPECT_EQ(last.front(), "line 95");
    fs::remove(p);
}

TEST(LogsCmd, LogPathFor_UsesKnownName) {
    auto p = log_path_for("gateway");
    EXPECT_NE(p.string().find("gateway.log"), std::string::npos);
    auto p2 = log_path_for("custom");
    EXPECT_NE(p2.string().find("custom.log"), std::string::npos);
}
