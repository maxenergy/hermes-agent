// Tests for the Phase-13 long-tail CLI subcommands: model switch /
// providers / memory / dump / webhook / runtime.
//
// These commands read/write ~/.hermes/config.yaml, so every test creates a
// scoped HERMES_HOME via a temp dir to avoid polluting the user's real
// config.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "hermes/cli/main_entry.hpp"
#include "hermes/config/loader.hpp"

namespace fs = std::filesystem;

namespace {

class ScopedHome {
public:
    ScopedHome() {
        tmp_ = fs::temp_directory_path() / ("hermes_longtail_" +
                                            std::to_string(::getpid()) + "_" +
                                            std::to_string(counter_++));
        fs::create_directories(tmp_);
        prev_ = std::getenv("HERMES_HOME") ? std::getenv("HERMES_HOME") : "";
        ::setenv("HERMES_HOME", tmp_.string().c_str(), 1);
    }
    ~ScopedHome() {
        if (prev_.empty()) ::unsetenv("HERMES_HOME");
        else ::setenv("HERMES_HOME", prev_.c_str(), 1);
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }
    const fs::path& dir() const { return tmp_; }

private:
    fs::path tmp_;
    std::string prev_;
    static inline int counter_ = 0;
};

// argv helper — array of const char* cast to char* for C compatibility.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;
    explicit Argv(std::initializer_list<std::string> args) : storage(args) {
        for (auto& s : storage) ptrs.push_back(s.data());
    }
    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// Silence stdout during a block.
class StdoutCapture {
public:
    StdoutCapture() : old_(std::cout.rdbuf(ss_.rdbuf())) {}
    ~StdoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return ss_.str(); }

private:
    std::stringstream ss_;
    std::streambuf* old_;
};

}  // namespace

TEST(ModelSwitch, UpdatesConfigModelField) {
    ScopedHome h;
    Argv v{"hermes", "model", "switch", "anthropic:claude-sonnet-4"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_model_switch(v.argc(), v.argv()), 0);

    auto cfg = hermes::config::load_cli_config();
    ASSERT_TRUE(cfg.contains("model"));
    EXPECT_EQ(cfg["model"], "anthropic:claude-sonnet-4");
}

TEST(ModelSwitch, MissingArgReturnsError) {
    Argv v{"hermes", "model", "switch"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_model_switch(v.argc(), v.argv()), 1);
}

TEST(Providers, ListPrintsKnownProviders) {
    Argv v{"hermes", "providers", "list"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 0);
    auto s = cap.str();
    EXPECT_NE(s.find("OpenRouter"), std::string::npos);
    EXPECT_NE(s.find("Anthropic"), std::string::npos);
    EXPECT_NE(s.find("ELEVENLABS_API_KEY"), std::string::npos);
}

TEST(Providers, UnknownActionFails) {
    Argv v{"hermes", "providers", "ship-it"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 1);
}

TEST(Dump, ConfigOutputsJson) {
    ScopedHome h;
    Argv v{"hermes", "dump", "config"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 0);
    auto s = cap.str();
    EXPECT_NE(s.find("_config_version"), std::string::npos);
}

TEST(Dump, SessionsHandlesEmptyDirectory) {
    ScopedHome h;
    Argv v{"hermes", "dump", "sessions"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 0);
    EXPECT_NE(cap.str().find("[]"), std::string::npos);
}

TEST(Dump, UnknownTargetReturnsError) {
    Argv v{"hermes", "dump", "everything"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 1);
}

TEST(Webhook, InstallWritesSecretsToConfig) {
    ScopedHome h;
    Argv v{"hermes", "webhook", "install"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 0);

    auto cfg = hermes::config::load_cli_config();
    EXPECT_TRUE(cfg.contains("platforms"));
    EXPECT_TRUE(cfg["platforms"]["api_server"]["signature_secret"].is_string());
    EXPECT_EQ(cfg["platforms"]["api_server"]["signature_secret"].get<std::string>().size(), 64u);
}

TEST(Runtime, TerminalSetsBackend) {
    ScopedHome h;
    Argv v{"hermes", "runtime", "terminal", "docker"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 0);
    auto cfg = hermes::config::load_cli_config();
    EXPECT_EQ(cfg["terminal"]["backend"], "docker");
}

TEST(Runtime, RejectsInvalidBackend) {
    ScopedHome h;
    Argv v{"hermes", "runtime", "terminal", "badbackend"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 1);
}

TEST(Runtime, MissingArgReturnsError) {
    Argv v{"hermes", "runtime"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 1);
}
