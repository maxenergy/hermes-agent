#include "hermes/auth/credentials.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ha = hermes::auth;
namespace fs = std::filesystem;

namespace {

class TempHermesHome {
public:
    TempHermesHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-auth-test-" + std::to_string(::getpid()) + "-" +
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

TEST(Credentials, StoreAndGetRoundTrip) {
    TempHermesHome home;
    ha::store_credential("OPENROUTER_API_KEY", "sk-test-123");
    auto got = ha::get_credential("OPENROUTER_API_KEY");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "sk-test-123");
}

TEST(Credentials, StoreUpdatesExistingKey) {
    TempHermesHome home;
    ha::store_credential("KEY_A", "first");
    ha::store_credential("KEY_A", "second");
    auto got = ha::get_credential("KEY_A");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "second");
    // There must still be exactly one KEY_A line in the file.
    const auto keys = ha::list_credential_keys();
    EXPECT_EQ(std::count(keys.begin(), keys.end(), "KEY_A"), 1);
}

TEST(Credentials, ClearPreservesOtherKeys) {
    TempHermesHome home;
    ha::store_credential("KEY_A", "a");
    ha::store_credential("KEY_B", "b");
    ha::store_credential("KEY_C", "c");
    ha::clear_credential("KEY_B");
    const auto keys = ha::list_credential_keys();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), "KEY_A"), 1);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), "KEY_C"), 1);
    EXPECT_FALSE(ha::get_credential("KEY_B").has_value());
    EXPECT_EQ(ha::get_credential("KEY_A").value_or(""), "a");
}

TEST(Credentials, ClearAllRemovesFile) {
    TempHermesHome home;
    ha::store_credential("KEY_X", "x");
    ASSERT_TRUE(fs::exists(home.path() / ".env"));
    ha::clear_all_credentials();
    EXPECT_FALSE(fs::exists(home.path() / ".env"));
    EXPECT_TRUE(ha::list_credential_keys().empty());
}

TEST(Credentials, ListKeysReturnsAll) {
    TempHermesHome home;
    ha::store_credential("ALPHA", "1");
    ha::store_credential("BETA", "2");
    const auto keys = ha::list_credential_keys();
    EXPECT_EQ(keys.size(), 2u);
}

TEST(Credentials, GetFallsBackToProcessEnv) {
    TempHermesHome home;
    ::setenv("HERMES_TEST_PROC_ENV", "shell-value", 1);
    auto got = ha::get_credential("HERMES_TEST_PROC_ENV");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "shell-value");
    ::unsetenv("HERMES_TEST_PROC_ENV");
}

#ifndef _WIN32
TEST(Credentials, FilePermissionsAre0600) {
    TempHermesHome home;
    ha::store_credential("KEY_Z", "z");
    struct stat st {};
    ASSERT_EQ(::stat((home.path() / ".env").c_str(), &st), 0);
    const auto perms = st.st_mode & 0777;
    EXPECT_EQ(perms, 0600) << "expected 0600, got 0" << std::oct << perms;
}
#endif

TEST(Credentials, QuotedValuesSurviveRoundTrip) {
    TempHermesHome home;
    ha::store_credential("KEY_WS", "has spaces and #hash");
    auto got = ha::get_credential("KEY_WS");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "has spaces and #hash");
}
