#include "hermes/config/default_config.hpp"
#include "hermes/config/loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

// Upstream: hermes_cli/config.py — ``_preserve_env_ref_templates`` / ``save_config``
// (commit 04a0c3cb). An atomic rewrite must not expand ``${VAR}`` references —
// they are meant to be re-resolved at load time.

namespace hc = hermes::config;
namespace fs = std::filesystem;

namespace {

class TempHermesHome {
public:
    TempHermesHome() {
        auto base = fs::temp_directory_path() /
                    ("hermes-config-envref-" + std::to_string(::getpid()) + "-" +
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

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(SaveConfigEnvRefs, PreservesTemplateAcrossLoadSaveRoundTrip) {
    TempHermesHome home;
    ::setenv("HERMES_ENVREF_FOO", "resolved-secret", 1);

    {
        std::ofstream f(home.path() / "config.yaml");
        f << "model: \"${HERMES_ENVREF_FOO}\"\n"
             "terminal:\n"
             "  backend: local\n";
    }

    auto cfg = hc::load_cli_config();
    // At runtime load returns the expanded value.
    EXPECT_EQ(cfg["model"].get<std::string>(), "resolved-secret");

    // Mutate an *unrelated* field and save.
    cfg["terminal"]["backend"] = "docker";
    hc::save_config(cfg);

    const std::string contents = slurp(home.path() / "config.yaml");
    // The raw ``${VAR}`` must still be on disk.
    EXPECT_NE(contents.find("${HERMES_ENVREF_FOO}"), std::string::npos)
        << "save_config expanded the env reference; contents=\n"
        << contents;
    // And the expanded secret must not be written.
    EXPECT_EQ(contents.find("resolved-secret"), std::string::npos)
        << "save_config persisted the expanded secret; contents=\n"
        << contents;
    // Unrelated edit made it through.
    EXPECT_NE(contents.find("docker"), std::string::npos);

    ::unsetenv("HERMES_ENVREF_FOO");
}

TEST(SaveConfigEnvRefs, ExplicitEditToSameFieldWins) {
    TempHermesHome home;
    ::setenv("HERMES_ENVREF_BAR", "secret-v1", 1);

    {
        std::ofstream f(home.path() / "config.yaml");
        f << "model: \"${HERMES_ENVREF_BAR}\"\n";
    }

    auto cfg = hc::load_cli_config();
    // Caller explicitly overrides the expanded value with a different literal.
    cfg["model"] = "literal-override";
    hc::save_config(cfg);

    const std::string contents = slurp(home.path() / "config.yaml");
    EXPECT_NE(contents.find("literal-override"), std::string::npos);
    EXPECT_EQ(contents.find("${HERMES_ENVREF_BAR}"), std::string::npos);

    ::unsetenv("HERMES_ENVREF_BAR");
}
