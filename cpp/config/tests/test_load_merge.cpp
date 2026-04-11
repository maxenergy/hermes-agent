#include "hermes/config/default_config.hpp"
#include "hermes/config/loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace hc = hermes::config;
namespace fs = std::filesystem;

namespace {

// RAII HERMES_HOME pointed at a tmp dir.  We create a unique dir per
// test so parallel ctest runs do not clash.
class TempHermesHome {
public:
    TempHermesHome() {
        auto base = fs::temp_directory_path() /
                    ("hermes-config-test-" + std::to_string(::getpid()) + "-" +
                     std::to_string(++counter_));
        fs::create_directories(base);
        dir_ = base;
        if (const char* old = std::getenv("HERMES_HOME"); old != nullptr) {
            had_old_ = true;
            old_ = old;
        }
        ::setenv("HERMES_HOME", dir_.c_str(), 1);
    }
    ~TempHermesHome() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        if (had_old_) {
            ::setenv("HERMES_HOME", old_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
    }
    const fs::path& path() const { return dir_; }

private:
    fs::path dir_;
    bool had_old_ = false;
    std::string old_;
    static inline int counter_ = 0;
};

}  // namespace

TEST(LoadCliConfig, MissingFileYieldsDefaults) {
    TempHermesHome home;
    auto cfg = hc::load_cli_config();
    EXPECT_EQ(cfg["_config_version"].get<int>(), hc::kCurrentConfigVersion);
    EXPECT_EQ(cfg["terminal"]["backend"].get<std::string>(), "local");
}

TEST(LoadCliConfig, OverlayMergesOverDefaults) {
    TempHermesHome home;
    {
        std::ofstream f(home.path() / "config.yaml");
        f << "model: gpt-5\n"
             "terminal:\n"
             "  backend: docker\n";
    }
    auto cfg = hc::load_cli_config();
    EXPECT_EQ(cfg["model"].get<std::string>(), "gpt-5");
    EXPECT_EQ(cfg["terminal"]["backend"].get<std::string>(), "docker");
    // Unspecified keys in the nested dict must survive from defaults.
    EXPECT_EQ(cfg["terminal"]["timeout"].get<int>(), 180);
    EXPECT_TRUE(cfg["terminal"].contains("use_pty"));
}

TEST(SaveConfig, RoundTrip) {
    TempHermesHome home;
    nlohmann::json cfg = hc::default_config();
    cfg["model"] = "claude-custom";
    cfg["tools"]["enabled_toolsets"] = nlohmann::json::array({"x", "y"});

    hc::save_config(cfg);
    ASSERT_TRUE(fs::exists(home.path() / "config.yaml"));

    auto loaded = hc::load_cli_config();
    EXPECT_EQ(loaded["model"].get<std::string>(), "claude-custom");
    ASSERT_TRUE(loaded["tools"]["enabled_toolsets"].is_array());
    EXPECT_EQ(loaded["tools"]["enabled_toolsets"].size(), 2u);
    EXPECT_EQ(loaded["tools"]["enabled_toolsets"][0].get<std::string>(), "x");
}

TEST(LoadCliConfig, StringValuesGetEnvExpanded) {
    TempHermesHome home;
    ::setenv("HERMES_TEST_EXPANDED", "ACTIVATED", 1);
    {
        std::ofstream f(home.path() / "config.yaml");
        f << "model: \"${HERMES_TEST_EXPANDED}\"\n";
    }
    auto cfg = hc::load_cli_config();
    EXPECT_EQ(cfg["model"].get<std::string>(), "ACTIVATED");
    ::unsetenv("HERMES_TEST_EXPANDED");
}
