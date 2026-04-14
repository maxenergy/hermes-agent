// Tests for hermes::cli::callbacks (non-interactive paths).
#include "hermes/cli/callbacks.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace hermes::cli::callbacks;
namespace fs = std::filesystem;

namespace {

class HermesHomeFixture {
public:
    HermesHomeFixture() {
        tmp_ = fs::temp_directory_path() /
               ("hermes_cb_test_" + std::to_string(rand()));
        fs::create_directories(tmp_);
        ::setenv("HERMES_HOME", tmp_.c_str(), 1);
    }
    ~HermesHomeFixture() {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        ::unsetenv("HERMES_HOME");
    }
    const fs::path& path() const { return tmp_; }

private:
    fs::path tmp_;
};

}  // namespace

TEST(Callbacks, SaveAndReadEnvValue) {
    HermesHomeFixture fx;
    EXPECT_TRUE(save_env_value_secure("MY_TOKEN", "abc123"));
    EXPECT_EQ(read_env_value("MY_TOKEN"), "abc123");
}

TEST(Callbacks, SaveEnvValue_OverwritesExisting) {
    HermesHomeFixture fx;
    save_env_value_secure("KEY1", "v1");
    save_env_value_secure("KEY2", "v2");
    save_env_value_secure("KEY1", "updated");
    EXPECT_EQ(read_env_value("KEY1"), "updated");
    EXPECT_EQ(read_env_value("KEY2"), "v2");
}

TEST(Callbacks, ReadEnvValue_MissingKey_ReturnsEmpty) {
    HermesHomeFixture fx;
    EXPECT_EQ(read_env_value("NEVER_SET"), "");
}

TEST(Callbacks, EnvFilePermissionsBestEffort) {
    HermesHomeFixture fx;
    save_env_value_secure("K", "v");
    auto p = fx.path() / ".env";
    EXPECT_TRUE(fs::exists(p));
    auto perms = fs::status(p).permissions();
    // Best-effort chmod 600 on POSIX — group/other should be clear.
#ifndef _WIN32
    using fs::perms;
    EXPECT_EQ(perms::none, (perms::group_read | perms::group_write |
                             perms::others_read | perms::others_write) &
                           perms);
#endif
}

TEST(Callbacks, ApprovalMutexIsSingleInstance) {
    auto& m1 = approval_mutex();
    auto& m2 = approval_mutex();
    EXPECT_EQ(&m1, &m2);
}

TEST(Callbacks, ClarifyResultStructHasDefaults) {
    ClarifyResult r;
    EXPECT_FALSE(r.timed_out);
    EXPECT_EQ(r.choice_index, -1);
    EXPECT_TRUE(r.response.empty());
}
