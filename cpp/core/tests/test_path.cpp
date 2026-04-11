#include "hermes/core/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace hp = hermes::core::path;
namespace fs = std::filesystem;

namespace {

class ScopedEnv {
public:
    ScopedEnv(std::string key, const char* value) : key_(std::move(key)) {
        if (const char* old = std::getenv(key_.c_str()); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        if (value == nullptr) {
            ::unsetenv(key_.c_str());
        } else {
            ::setenv(key_.c_str(), value, 1);
        }
    }
    ~ScopedEnv() {
        if (had_old_) {
            ::setenv(key_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string key_;
    bool had_old_ = false;
    std::string old_;
};

}  // namespace

TEST(Path, HermesHomeFromEnv) {
    ScopedEnv guard("HERMES_HOME", "/tmp/hermes-home-test");
    EXPECT_EQ(hp::get_hermes_home(), fs::path("/tmp/hermes-home-test"));
}

TEST(Path, HermesHomeDefaultsToHomeDotHermes) {
    ScopedEnv guard("HERMES_HOME", nullptr);
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(hp::get_hermes_home(), fs::path(home) / ".hermes");
}

TEST(Path, DefaultHermesRootIgnoresEnv) {
    ScopedEnv guard("HERMES_HOME", "/opt/something");
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(hp::get_default_hermes_root(), fs::path(home) / ".hermes");
}

TEST(Path, ProfilesRootAlwaysHomeAnchored) {
    ScopedEnv guard("HERMES_HOME", "/opt/something");
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(hp::get_profiles_root(), fs::path(home) / ".hermes" / "profiles");
}

TEST(Path, GetHermesDirBackwardCompat) {
    // Create a throwaway HERMES_HOME with only the legacy subdir.
    auto tmp = fs::temp_directory_path() / "hermes-backcompat-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "image_cache");
    ScopedEnv guard("HERMES_HOME", tmp.c_str());

    EXPECT_EQ(hp::get_hermes_dir("cache/images", "image_cache"),
              tmp / "image_cache");

    // When neither exists, prefer the new location.
    EXPECT_EQ(hp::get_hermes_dir("cache/audio", "audio_cache"),
              tmp / "cache/audio");

    fs::remove_all(tmp);
}

TEST(Path, OptionalSkillsDir) {
    ScopedEnv guard("HERMES_HOME", "/tmp/hermes-skill-test");
    EXPECT_EQ(hp::get_optional_skills_dir(),
              fs::path("/tmp/hermes-skill-test") / "optional-skills");
}

TEST(Path, DisplayHermesHomeShorthand) {
    ScopedEnv guard("HERMES_HOME", nullptr);
    const std::string display = hp::display_hermes_home();
    // Expect ~/.hermes or an absolute path — at minimum non-empty.
    EXPECT_FALSE(display.empty());
}
