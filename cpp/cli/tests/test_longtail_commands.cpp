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

#include "hermes/auth/credentials.hpp"
#include "hermes/cli/main_entry.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/llm/llm_client.hpp"

#include <nlohmann/json.hpp>

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

// Redirect std::cin from a preloaded string — used to feed API-key prompts.
class StdinFeed {
public:
    explicit StdinFeed(std::string text) : ss_(std::move(text)),
                                            old_(std::cin.rdbuf(ss_.rdbuf())) {}
    ~StdinFeed() { std::cin.rdbuf(old_); }

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

// -----------------------------------------------------------------------------
// auth — API-key providers (openai/anthropic/nous) land in <HERMES_HOME>/.env.
// OAuth providers (qwen/copilot) exercise the aggregate status path only since
// a real device-code flow would need a network and human input.
// -----------------------------------------------------------------------------

TEST(Auth, LoginOpenaiStoresCredential) {
    ScopedHome h;
    StdinFeed stdin_feed("sk-test-1234567890abcdef\n");
    Argv v{"hermes", "auth", "login", "openai"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 0);

    auto got = hermes::auth::get_credential("OPENAI_API_KEY");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "sk-test-1234567890abcdef");
}

TEST(Auth, LogoutOpenaiClearsCredential) {
    ScopedHome h;
    hermes::auth::store_credential("OPENAI_API_KEY", "sk-stored");
    Argv v{"hermes", "auth", "logout", "openai"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 0);

    auto got = hermes::auth::get_credential("OPENAI_API_KEY");
    // Falls back to env; in a scoped home with no env var set, must be unset.
    ::unsetenv("OPENAI_API_KEY");
    got = hermes::auth::get_credential("OPENAI_API_KEY");
    EXPECT_FALSE(got.has_value());
}

TEST(Auth, StatusAggregateIncludesAllProviders) {
    ScopedHome h;
    Argv v{"hermes", "auth", "status"};
    StdoutCapture cap;
    // Return value may be 0 even when nothing is configured — the aggregate
    // status always succeeds, it just reports the absence.
    hermes::cli::cmd_auth(v.argc(), v.argv());
    auto s = cap.str();
    EXPECT_NE(s.find("qwen"), std::string::npos);
    EXPECT_NE(s.find("copilot"), std::string::npos);
    EXPECT_NE(s.find("openai"), std::string::npos);
    EXPECT_NE(s.find("anthropic"), std::string::npos);
    EXPECT_NE(s.find("nous"), std::string::npos);
}

TEST(Auth, StatusAnthropicReportsConfigured) {
    ScopedHome h;
    hermes::auth::store_credential("ANTHROPIC_API_KEY", "sk-ant-abcd1234efgh");
    Argv v{"hermes", "auth", "status", "anthropic"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 0);
    auto s = cap.str();
    EXPECT_NE(s.find("anthropic"), std::string::npos);
    EXPECT_NE(s.find("ANTHROPIC_API_KEY"), std::string::npos);
}

TEST(Auth, StatusOpenaiReturnsOneWhenUnset) {
    ScopedHome h;
    ::unsetenv("OPENAI_API_KEY");
    Argv v{"hermes", "auth", "status", "openai"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 1);
}

TEST(Auth, UnknownProviderReturnsError) {
    ScopedHome h;
    Argv v{"hermes", "auth", "login", "bogus"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 1);
}

TEST(Auth, BackCompatProviderFirstForm) {
    // `hermes auth openai status` — legacy ordering should still work.
    ScopedHome h;
    hermes::auth::store_credential("OPENAI_API_KEY", "sk-legacy");
    Argv v{"hermes", "auth", "openai", "status"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_auth(v.argc(), v.argv()), 0);
    EXPECT_NE(cap.str().find("openai"), std::string::npos);
}

// -----------------------------------------------------------------------------
// webhook — URL-install to ~/.hermes/webhooks.json.
// -----------------------------------------------------------------------------

TEST(Webhook, InstallUrlWritesWebhooksJson) {
    ScopedHome h;
    Argv v{"hermes", "webhook", "install", "https://example.com/hook",
           "--secret", "shhh-1234", "--name", "test"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 0);

    auto path = h.dir() / "webhooks.json";
    ASSERT_TRUE(fs::exists(path));
    std::ifstream f(path);
    auto j = nlohmann::json::parse(f);
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["name"], "test");
    EXPECT_EQ(j[0]["url"], "https://example.com/hook");
    EXPECT_EQ(j[0]["secret"], "shhh-1234");
}

TEST(Webhook, InstallRejectsInvalidUrl) {
    ScopedHome h;
    Argv v{"hermes", "webhook", "install", "not-a-url"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 1);
}

TEST(Webhook, ListPrintsRegistered) {
    ScopedHome h;
    {
        Argv v{"hermes", "webhook", "install", "https://a.example/x",
               "--name", "alpha"};
        StdoutCapture cap;
        hermes::cli::cmd_webhook(v.argc(), v.argv());
    }
    Argv v{"hermes", "webhook", "--list"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 0);
    EXPECT_NE(cap.str().find("alpha"), std::string::npos);
    EXPECT_NE(cap.str().find("https://a.example/x"), std::string::npos);
}

TEST(Webhook, RemoveDeletesEntry) {
    ScopedHome h;
    {
        Argv v{"hermes", "webhook", "install", "https://a.example/x",
               "--name", "rm-me"};
        StdoutCapture cap;
        hermes::cli::cmd_webhook(v.argc(), v.argv());
    }
    Argv v{"hermes", "webhook", "--remove", "rm-me"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 0);

    auto path = h.dir() / "webhooks.json";
    std::ifstream f(path);
    auto j = nlohmann::json::parse(f);
    EXPECT_EQ(j.size(), 0u);
}

TEST(Webhook, RemoveUnknownNameFails) {
    ScopedHome h;
    Argv v{"hermes", "webhook", "--remove", "nope"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_webhook(v.argc(), v.argv()), 1);
}

// -----------------------------------------------------------------------------
// dump — --since filter, --output redirection, config redaction.
// -----------------------------------------------------------------------------

TEST(Dump, ConfigRedactsSecrets) {
    ScopedHome h;
    auto cfg = hermes::config::load_cli_config();
    cfg["provider_api_key"] = "sk-SENSITIVE-12345";
    cfg["platforms"]["slack"]["signing_secret"] = "shh";
    hermes::config::save_config(cfg);

    Argv v{"hermes", "dump", "config"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 0);
    auto s = cap.str();
    EXPECT_EQ(s.find("sk-SENSITIVE-12345"), std::string::npos);
    EXPECT_EQ(s.find("\"shh\""), std::string::npos);
    EXPECT_NE(s.find("***"), std::string::npos);
}

TEST(Dump, SessionsWritesToOutputFile) {
    ScopedHome h;
    // Create a single fake session.
    auto sdir = h.dir() / "sessions" / "sess1";
    fs::create_directories(sdir);
    {
        std::ofstream f(sdir / "session.json");
        f << R"({"id":"sess1","updated_at":100})";
    }
    auto out = h.dir() / "out.jsonl";
    Argv v{"hermes", "dump", "sessions", "--output", out.string()};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 0);
    ASSERT_TRUE(fs::exists(out));
    std::ifstream f(out);
    std::string line;
    std::getline(f, line);
    EXPECT_NE(line.find("sess1"), std::string::npos);
}

TEST(Dump, SessionsSinceFiltersOlder) {
    ScopedHome h;
    // Old session (updated_at = 100).
    {
        auto d = h.dir() / "sessions" / "old";
        fs::create_directories(d);
        std::ofstream(d / "session.json") << R"({"id":"old","updated_at":100})";
    }
    // New session (updated_at well in the future).
    {
        auto d = h.dir() / "sessions" / "new";
        fs::create_directories(d);
        std::ofstream(d / "session.json")
            << R"({"id":"new","updated_at":99999999999})";
    }
    auto out = h.dir() / "filtered.jsonl";
    Argv v{"hermes", "dump", "sessions", "--since", "2099-01-01",
           "--output", out.string()};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 0);
    std::ifstream f(out);
    std::stringstream buf; buf << f.rdbuf();
    auto s = buf.str();
    EXPECT_EQ(s.find("\"old\""), std::string::npos);
    EXPECT_NE(s.find("\"new\""), std::string::npos);
}

TEST(Dump, RejectsInvalidSince) {
    ScopedHome h;
    Argv v{"hermes", "dump", "sessions", "--since", "not-a-date"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_dump(v.argc(), v.argv()), 1);
}

// -----------------------------------------------------------------------------
// providers test — uses injected FakeHttpTransport so no network hit.
// -----------------------------------------------------------------------------

TEST(Providers, TestReturnsZeroOnReachable) {
    ScopedHome h;
    hermes::auth::store_credential("OPENAI_API_KEY", "sk-fake");

    hermes::llm::FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"data":[]})", {}});
    hermes::cli::set_providers_transport_override(&fake);

    Argv v{"hermes", "providers", "test", "openai"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 0);
    EXPECT_NE(cap.str().find("reachable"), std::string::npos);

    hermes::cli::set_providers_transport_override(nullptr);
}

TEST(Providers, TestRequiresApiKey) {
    ScopedHome h;
    ::unsetenv("ANTHROPIC_API_KEY");
    Argv v{"hermes", "providers", "test", "anthropic"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 1);
}

TEST(Providers, TestUnknownProviderFails) {
    ScopedHome h;
    Argv v{"hermes", "providers", "test", "nosuch"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 1);
}

TEST(Providers, TestReportsUnreachableOn500) {
    ScopedHome h;
    hermes::auth::store_credential("GROQ_API_KEY", "gsk-fake");

    hermes::llm::FakeHttpTransport fake;
    fake.enqueue_response({500, "oops", {}});
    hermes::cli::set_providers_transport_override(&fake);

    Argv v{"hermes", "providers", "test", "groq"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_providers(v.argc(), v.argv()), 1);
    EXPECT_NE(cap.str().find("unreachable"), std::string::npos);

    hermes::cli::set_providers_transport_override(nullptr);
}

// -----------------------------------------------------------------------------
// runtime — list & select.
// -----------------------------------------------------------------------------

TEST(Runtime, ListShowsBackendsWithActiveMarker) {
    ScopedHome h;
    // First switch to docker so we can check the "*" marker.
    {
        Argv v{"hermes", "runtime", "terminal", "docker"};
        StdoutCapture cap;
        hermes::cli::cmd_runtime(v.argc(), v.argv());
    }
    Argv v{"hermes", "runtime", "list"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 0);
    auto s = cap.str();
    EXPECT_NE(s.find("docker"), std::string::npos);
    EXPECT_NE(s.find("local"), std::string::npos);
    EXPECT_NE(s.find("* docker"), std::string::npos);
}

TEST(Runtime, SelectAliasSetsBackend) {
    ScopedHome h;
    Argv v{"hermes", "runtime", "select", "modal"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 0);
    auto cfg = hermes::config::load_cli_config();
    EXPECT_EQ(cfg["terminal"]["backend"], "modal");
}

TEST(Runtime, SelectRejectsInvalidBackend) {
    ScopedHome h;
    Argv v{"hermes", "runtime", "select", "wat"};
    StdoutCapture cap;
    EXPECT_EQ(hermes::cli::cmd_runtime(v.argc(), v.argv()), 1);
}
