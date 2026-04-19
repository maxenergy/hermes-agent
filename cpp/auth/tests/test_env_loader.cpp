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

// Upstream: trajectory_compressor.py (commit 4b47856f). ``.env``
// lookup order must be ``<HERMES_HOME>/.env`` first, project-root
// ``./.env`` fallback — the trajectory pipeline ran outside
// ``hermes_cli`` and accidentally inverted that order, causing stale
// credentials from the checkout to win over the profile-scoped ones.
TEST(EnvLoader, HermesHomeBeatsProjectRoot) {
    // Create a project-style CWD with a competing .env, then point
    // HERMES_HOME at a *different* directory whose .env sets the same
    // key to a different value.  The HERMES_HOME entry must win.
    auto project_dir =
        fs::temp_directory_path() /
        ("hermes-env-cwd-" + std::to_string(::getpid()) + "-bp");
    fs::create_directories(project_dir);
    {
        std::ofstream f(project_dir / ".env");
        f << "HERMES_TEST_SCOPE=project-value\n";
    }

    const auto prev_cwd = fs::current_path();
    fs::current_path(project_dir);

    ::unsetenv("HERMES_TEST_SCOPE");

    {
        TempHermesHome home;  // also sets HERMES_HOME env var
        {
            std::ofstream f(home.path() / ".env");
            f << "HERMES_TEST_SCOPE=hermes-home-value\n";
        }
        ha::load_profile_env();
        const char* got = std::getenv("HERMES_TEST_SCOPE");
        ASSERT_NE(got, nullptr);
        EXPECT_STREQ(got, "hermes-home-value")
            << "HERMES_HOME/.env was clobbered by CWD/.env";
    }

    ::unsetenv("HERMES_TEST_SCOPE");
    fs::current_path(prev_cwd);
    std::error_code ec;
    fs::remove_all(project_dir, ec);
}

TEST(EnvLoader, HermesHomeEnvVarIsHonoured) {
    // Direct test of the spec: ``std::getenv("HERMES_HOME")`` is the
    // discriminator — pointing it at a temp dir with a .env makes the
    // loader pick it up.
    auto base =
        fs::temp_directory_path() /
        ("hermes-env-honour-" + std::to_string(::getpid()) + "-hh");
    fs::create_directories(base);
    {
        std::ofstream f(base / ".env");
        f << "HERMES_TEST_HONOUR=via-hermes-home\n";
    }

    // Save + override HERMES_HOME.
    bool had_old = false;
    std::string old_home;
    if (const char* old = std::getenv("HERMES_HOME"); old != nullptr) {
        had_old = true;
        old_home = old;
    }
    ::setenv("HERMES_HOME", base.c_str(), 1);

    ::unsetenv("HERMES_TEST_HONOUR");
    ha::load_profile_env();
    const char* got = std::getenv("HERMES_TEST_HONOUR");
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got, "via-hermes-home");

    // Cleanup.
    ::unsetenv("HERMES_TEST_HONOUR");
    if (had_old) {
        ::setenv("HERMES_HOME", old_home.c_str(), 1);
    } else {
        ::unsetenv("HERMES_HOME");
    }
    std::error_code ec;
    fs::remove_all(base, ec);
}
