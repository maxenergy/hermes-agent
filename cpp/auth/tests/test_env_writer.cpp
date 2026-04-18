#include "hermes/auth/env_writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ha = hermes::auth;
namespace fs = std::filesystem;

namespace {

// Lightweight scoped HERMES_HOME override + temp-dir cleanup.  Pattern
// mirrors test_env_loader.cpp's `TempHermesHome` so the two tests look
// the same when read side-by-side.
class TempHermesHome {
public:
    TempHermesHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-envwriter-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++counter_));
        fs::create_directories(base_);
        if (const char* old = std::getenv("HERMES_HOME"); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        ::setenv("HERMES_HOME", base_.c_str(), 1);
    }
    ~TempHermesHome() {
        std::error_code ec;
        fs::remove_all(base_, ec);
        if (had_old_) {
            ::setenv("HERMES_HOME", old_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
    }
    const fs::path& path() const { return base_; }

private:
    fs::path base_;
    bool had_old_ = false;
    std::string old_;
    static inline int counter_ = 0;
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

}  // namespace

// ---------------------------------------------------------------------------
// rewrite_env_file — happy path: mixed update / remove / preserve
// ---------------------------------------------------------------------------
TEST(EnvWriter, RewriteUpdatesRemovesPreservesLayout) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";

    const std::string initial =
        "# header comment\n"
        "\n"
        "KEEP_ME=untouched\n"
        "UPDATE_ME=old-value\n"
        "# mid comment\n"
        "export EXPORTED_KEY=\"quoted value\"\n"
        "\n"
        "REMOVE_ME=bye\n"
        "# trailing comment\n";
    write_file(env_path, initial);

    std::unordered_map<std::string, std::string> updates = {
        {"UPDATE_ME", "new-value"},
        {"EXPORTED_KEY", "fresh"},
        {"APPENDED", "appended-value"},
    };
    std::unordered_set<std::string> removals = {"REMOVE_ME"};

    ASSERT_TRUE(ha::rewrite_env_file(env_path, updates, removals));

    const std::string result = read_file(env_path);

    // Header comment / blank / mid comment / trailing comment preserved.
    EXPECT_NE(result.find("# header comment\n"), std::string::npos);
    EXPECT_NE(result.find("\n\nKEEP_ME=untouched\n"), std::string::npos);
    EXPECT_NE(result.find("# mid comment\n"), std::string::npos);
    EXPECT_NE(result.find("# trailing comment\n"), std::string::npos);

    // Untouched line stays identical.
    EXPECT_NE(result.find("\nKEEP_ME=untouched\n"), std::string::npos);

    // Updated line replaces the value but keeps the key position (before
    // the mid-comment) — i.e. still comes before "# mid comment".
    const auto update_pos = result.find("UPDATE_ME=new-value\n");
    const auto mid_pos = result.find("# mid comment");
    ASSERT_NE(update_pos, std::string::npos);
    ASSERT_NE(mid_pos, std::string::npos);
    EXPECT_LT(update_pos, mid_pos);

    // `export ` prefix preserved across update.
    EXPECT_NE(result.find("export EXPORTED_KEY=fresh\n"), std::string::npos);

    // Old value is gone.
    EXPECT_EQ(result.find("old-value"), std::string::npos);
    EXPECT_EQ(result.find("\"quoted value\""), std::string::npos);

    // Removed line is gone entirely.
    EXPECT_EQ(result.find("REMOVE_ME"), std::string::npos);
    EXPECT_EQ(result.find("bye"), std::string::npos);

    // New key appended at the end.
    EXPECT_NE(result.find("APPENDED=appended-value\n"), std::string::npos);
    EXPECT_GT(result.find("APPENDED=appended-value"),
              result.find("# trailing comment"));
}

// ---------------------------------------------------------------------------
// rewrite_env_file — removal matches `export KEY=...` form
// ---------------------------------------------------------------------------
TEST(EnvWriter, RemovesExportPrefixedKey) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    write_file(env_path,
               "export STALE=sk-xxx\n"
               "KEEPER=1\n");

    ASSERT_TRUE(ha::rewrite_env_file(env_path, /*updates=*/{}, {"STALE"}));

    const std::string result = read_file(env_path);
    EXPECT_EQ(result.find("STALE"), std::string::npos);
    EXPECT_NE(result.find("KEEPER=1\n"), std::string::npos);
}

// ---------------------------------------------------------------------------
// rewrite_env_file — key with quoted value is recognised for removal
// ---------------------------------------------------------------------------
TEST(EnvWriter, RemovesKeyWithQuotedValue) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    write_file(env_path,
               "QUOTED_VAR=\"value with = inside and spaces\"\n"
               "OTHER=2\n");

    ASSERT_TRUE(ha::rewrite_env_file(env_path, /*updates=*/{}, {"QUOTED_VAR"}));

    const std::string result = read_file(env_path);
    EXPECT_EQ(result.find("QUOTED_VAR"), std::string::npos);
    EXPECT_EQ(result.find("value with = inside"), std::string::npos);
    EXPECT_NE(result.find("OTHER=2\n"), std::string::npos);
}

// ---------------------------------------------------------------------------
// clear_env_value — missing key is a no-op (returns success)
// ---------------------------------------------------------------------------
TEST(EnvWriter, ClearMissingKeyIsNoOp) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    write_file(env_path, "PRESENT=yes\n# comment\n");
    const std::string before = read_file(env_path);

    EXPECT_TRUE(ha::clear_env_value("ABSENT_KEY"));

    const std::string after = read_file(env_path);
    EXPECT_EQ(before, after);
}

// ---------------------------------------------------------------------------
// rewrite_env_file — file does not exist yet
// ---------------------------------------------------------------------------
TEST(EnvWriter, RewriteCreatesFileWhenMissingWithUpdates) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    ASSERT_FALSE(fs::exists(env_path));

    std::unordered_map<std::string, std::string> updates = {
        {"NEW_KEY", "created"},
    };

    ASSERT_TRUE(ha::rewrite_env_file(env_path, updates, /*removals=*/{}));
    ASSERT_TRUE(fs::exists(env_path));

    const std::string result = read_file(env_path);
    EXPECT_EQ(result, "NEW_KEY=created\n");
}

TEST(EnvWriter, RewriteOnMissingFileWithOnlyRemovalsIsNoOp) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    ASSERT_FALSE(fs::exists(env_path));

    // Only removals, and the file doesn't exist → silent success, no
    // empty file created.
    EXPECT_TRUE(ha::rewrite_env_file(env_path, /*updates=*/{},
                                     {"FOO", "BAR"}));
    EXPECT_FALSE(fs::exists(env_path));
}

// ---------------------------------------------------------------------------
// set_env_value — convenience setter
// ---------------------------------------------------------------------------
TEST(EnvWriter, SetEnvValueCreatesAndUpdates) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";

    ASSERT_TRUE(ha::set_env_value("ROUND_TRIP", "first"));
    EXPECT_EQ(read_file(env_path), "ROUND_TRIP=first\n");

    ASSERT_TRUE(ha::set_env_value("ROUND_TRIP", "second"));
    EXPECT_EQ(read_file(env_path), "ROUND_TRIP=second\n");

    // Adding a second key preserves the first.
    ASSERT_TRUE(ha::set_env_value("SECOND_KEY", "also-here"));
    const std::string result = read_file(env_path);
    EXPECT_NE(result.find("ROUND_TRIP=second\n"), std::string::npos);
    EXPECT_NE(result.find("SECOND_KEY=also-here\n"), std::string::npos);
}

// ---------------------------------------------------------------------------
// rewrite_env_file — empty updates AND removals is a no-op
// ---------------------------------------------------------------------------
TEST(EnvWriter, EmptyUpdatesAndRemovalsIsNoOp) {
    TempHermesHome home;
    const auto env_path = home.path() / ".env";
    write_file(env_path, "X=1\n");
    const auto mtime_before = fs::last_write_time(env_path);

    EXPECT_TRUE(ha::rewrite_env_file(env_path, /*updates=*/{},
                                     /*removals=*/{}));

    const auto mtime_after = fs::last_write_time(env_path);
    EXPECT_EQ(mtime_before, mtime_after);  // file not rewritten
    EXPECT_EQ(read_file(env_path), "X=1\n");
}
