#include "hermes/core/path.hpp"
#include "hermes/profile/profile.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace hp = hermes::profile;
namespace fs = std::filesystem;

namespace {

// RAII HOME override — pins HOME to a tmp dir so the profiles root
// (which is HOME-anchored) lives entirely under our control.
class TempHome {
public:
    TempHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-profile-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++counter_));
        fs::create_directories(base_);

        if (const char* h = std::getenv("HOME"); h != nullptr) {
            had_home_ = true;
            old_home_ = h;
        }
        if (const char* h = std::getenv("HERMES_HOME"); h != nullptr) {
            had_hermes_home_ = true;
            old_hermes_home_ = h;
        }
        ::setenv("HOME", base_.c_str(), 1);
        ::unsetenv("HERMES_HOME");
    }
    ~TempHome() {
        std::error_code ec;
        fs::remove_all(base_, ec);
        if (had_home_) {
            ::setenv("HOME", old_home_.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
        if (had_hermes_home_) {
            ::setenv("HERMES_HOME", old_hermes_home_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
    }
    const fs::path& path() const { return base_; }

private:
    fs::path base_;
    bool had_home_ = false;
    std::string old_home_;
    bool had_hermes_home_ = false;
    std::string old_hermes_home_;
    static inline int counter_ = 0;
};

}  // namespace

TEST(Profile, ApplyOverrideSetsHermesHome) {
    TempHome home;
    hp::apply_profile_override(std::string("coder"));
    const char* h = std::getenv("HERMES_HOME");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(fs::path(h), home.path() / ".hermes" / "profiles" / "coder");
    // Directory should be created as a side effect.
    EXPECT_TRUE(fs::is_directory(h));
}

TEST(Profile, ApplyOverrideEmptyIsNoop) {
    TempHome home;
    ::unsetenv("HERMES_HOME");
    hp::apply_profile_override(std::string(""));
    EXPECT_EQ(std::getenv("HERMES_HOME"), nullptr);
    hp::apply_profile_override(std::nullopt);
    EXPECT_EQ(std::getenv("HERMES_HOME"), nullptr);
}

TEST(Profile, ListEmpty) {
    TempHome home;
    EXPECT_TRUE(hp::list_profiles().empty());
}

TEST(Profile, CreateAndListRoundTrip) {
    TempHome home;
    hp::create_profile("alpha");
    hp::create_profile("beta");
    auto names = hp::list_profiles();
    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "beta");
    EXPECT_TRUE(fs::exists(home.path() / ".hermes" / "profiles" / "alpha" /
                           "config.yaml"));
}

TEST(Profile, CreateSeedsFromDefaultConfig) {
    TempHome home;
    // Lay down a default config to be copied.
    const auto default_root = home.path() / ".hermes";
    fs::create_directories(default_root);
    {
        std::ofstream f(default_root / "config.yaml");
        f << "model: seed-value\n";
    }
    hp::create_profile("seeded");
    const auto dst =
        home.path() / ".hermes" / "profiles" / "seeded" / "config.yaml";
    ASSERT_TRUE(fs::exists(dst));
    std::ifstream f(dst);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("seed-value"), std::string::npos);
}

TEST(Profile, DeleteRemovesDir) {
    TempHome home;
    hp::create_profile("gone");
    EXPECT_EQ(hp::list_profiles().size(), 1u);
    hp::delete_profile("gone");
    EXPECT_TRUE(hp::list_profiles().empty());
    EXPECT_FALSE(fs::exists(home.path() / ".hermes" / "profiles" / "gone"));
}

TEST(Profile, RenameMovesDir) {
    TempHome home;
    hp::create_profile("old");
    hp::rename_profile("old", "shiny");
    auto names = hp::list_profiles();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "shiny");
}

TEST(Profile, DeleteActiveProfileThrows) {
    TempHome home;
    hp::create_profile("active-one");
    // Pin HERMES_HOME at the profile dir to make it the "active" profile.
    const auto active_dir =
        home.path() / ".hermes" / "profiles" / "active-one";
    ::setenv("HERMES_HOME", active_dir.c_str(), 1);
    EXPECT_THROW(hp::delete_profile("active-one"), std::runtime_error);
    EXPECT_THROW(hp::rename_profile("active-one", "nope"), std::runtime_error);
}

TEST(Profile, ProfilesRootStaysHomeAnchored) {
    TempHome home;
    hp::create_profile("one");
    // Apply override — this flips HERMES_HOME but must NOT move the
    // profiles root out from under `list_profiles`.
    hp::apply_profile_override(std::string("one"));
    const char* after = std::getenv("HERMES_HOME");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(fs::path(after), home.path() / ".hermes" / "profiles" / "one");
    // Profiles root is still HOME-anchored — it must ignore HERMES_HOME.
    EXPECT_EQ(hp::get_profiles_root(), home.path() / ".hermes" / "profiles");
    // And list_profiles still enumerates the one we created.
    const auto names = hp::list_profiles();
    EXPECT_EQ(names.size(), 1u);
}
