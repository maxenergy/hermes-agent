#include "hermes/tools/browser_camofox.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

using hermes::tools::BrowserCamofoxConfig;
using hermes::tools::BrowserCamofoxState;
using hermes::tools::camofox_available;
using hermes::tools::camofox_state_dir;
using hermes::tools::make_camofox_backend;
using hermes::tools::register_camofox_browser_backend;

namespace {

class CamofoxTest : public ::testing::Test {
protected:
    std::filesystem::path tmp;
    void SetUp() override {
        tmp = std::filesystem::temp_directory_path() /
              ("hermes_camofox_" + std::to_string(::getpid()));
        std::filesystem::remove_all(tmp);
        std::filesystem::create_directories(tmp);
        ::setenv("HERMES_HOME", tmp.c_str(), 1);
    }
    void TearDown() override {
        std::filesystem::remove_all(tmp);
        ::unsetenv("HERMES_HOME");
    }
};

TEST_F(CamofoxTest, StateDirLivesUnderHermesHome) {
    auto dir = camofox_state_dir();
    EXPECT_NE(dir.string().find("browser_auth"), std::string::npos);
    EXPECT_NE(dir.string().find("camofox"), std::string::npos);
}

TEST_F(CamofoxTest, AvailableIsFalseForMissingBinary) {
    EXPECT_FALSE(camofox_available("camofox-launcher-does-not-exist-xyz"));
}

TEST_F(CamofoxTest, MakeBackendReturnsNullWhenLauncherMissing) {
    BrowserCamofoxConfig cfg;
    cfg.launcher_path = "camofox-not-installed-xyz";
    auto b = make_camofox_backend(cfg);
    EXPECT_EQ(b.get(), nullptr);
}

TEST_F(CamofoxTest, RegisterReturnsFalseWhenLauncherMissing) {
    BrowserCamofoxConfig cfg;
    cfg.launcher_path = "camofox-not-installed-xyz";
    EXPECT_FALSE(register_camofox_browser_backend(cfg));
}

TEST_F(CamofoxTest, StateLoadMintsStableIdentity) {
    auto s1 = BrowserCamofoxState::load_for_task("t1");
    EXPECT_FALSE(s1.user_id.empty());
    EXPECT_FALSE(s1.session_key.empty());
    // Same task → same identity.
    auto s2 = BrowserCamofoxState::load_for_task("t1");
    EXPECT_EQ(s1.user_id, s2.user_id);
    EXPECT_EQ(s1.session_key, s2.session_key);
    // Different task → same user_id (profile-scoped) but different session.
    auto s3 = BrowserCamofoxState::load_for_task("t2");
    EXPECT_EQ(s1.user_id, s3.user_id);
    EXPECT_NE(s1.session_key, s3.session_key);
}

TEST_F(CamofoxTest, StateJsonRoundTrip) {
    BrowserCamofoxState s;
    s.user_id = "hermes_abc";
    s.session_key = "task_xyz";
    s.fingerprint_seed = "seed";
    s.cookies.push_back({{"name", "sid"}, {"value", "1"}});
    auto j = s.to_json();
    auto back = BrowserCamofoxState::from_json(j);
    EXPECT_EQ(back.user_id, "hermes_abc");
    EXPECT_EQ(back.session_key, "task_xyz");
    EXPECT_EQ(back.cookies.size(), 1u);
}

}  // namespace
