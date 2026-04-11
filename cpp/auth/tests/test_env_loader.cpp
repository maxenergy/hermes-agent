#include "hermes/auth/env_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace ha = hermes::auth;
namespace fs = std::filesystem;

namespace {

class TempHermesHome {
public:
    TempHermesHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-env-test-" + std::to_string(::getpid()) + "-" +
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

}  // namespace

TEST(EnvLoader, PicksUpPlainKeyValue) {
    TempHermesHome home;
    {
        std::ofstream f(home.path() / ".env");
        f << "HERMES_TEST_LOADER_KEY=loader-value\n";
    }
    ::unsetenv("HERMES_TEST_LOADER_KEY");
    ha::load_profile_env();
    const char* got = std::getenv("HERMES_TEST_LOADER_KEY");
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got, "loader-value");
    ::unsetenv("HERMES_TEST_LOADER_KEY");
}

TEST(EnvLoader, QuotedValuesArePreserved) {
    TempHermesHome home;
    {
        std::ofstream f(home.path() / ".env");
        f << "HERMES_TEST_QUOTED=\"hello world\"\n"
             "HERMES_TEST_SINGLE='don''t touch'\n";  // single-quoted body
    }
    ::unsetenv("HERMES_TEST_QUOTED");
    ::unsetenv("HERMES_TEST_SINGLE");
    ha::load_profile_env();
    ASSERT_NE(std::getenv("HERMES_TEST_QUOTED"), nullptr);
    EXPECT_STREQ(std::getenv("HERMES_TEST_QUOTED"), "hello world");
    ::unsetenv("HERMES_TEST_QUOTED");
    ::unsetenv("HERMES_TEST_SINGLE");
}

TEST(EnvLoader, CommentsAreSkipped) {
    TempHermesHome home;
    {
        std::ofstream f(home.path() / ".env");
        f << "# this is a comment\n"
             "\n"
             "HERMES_TEST_AFTER_COMMENT=ok\n";
    }
    ::unsetenv("HERMES_TEST_AFTER_COMMENT");
    ha::load_profile_env();
    ASSERT_NE(std::getenv("HERMES_TEST_AFTER_COMMENT"), nullptr);
    EXPECT_STREQ(std::getenv("HERMES_TEST_AFTER_COMMENT"), "ok");
    ::unsetenv("HERMES_TEST_AFTER_COMMENT");
}

TEST(EnvLoader, IsIdempotent) {
    TempHermesHome home;
    {
        std::ofstream f(home.path() / ".env");
        f << "HERMES_TEST_IDEMP=once\n";
    }
    ::unsetenv("HERMES_TEST_IDEMP");
    ha::load_profile_env();
    ha::load_profile_env();
    ha::load_profile_env();
    const char* got = std::getenv("HERMES_TEST_IDEMP");
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got, "once");
    ::unsetenv("HERMES_TEST_IDEMP");
}
