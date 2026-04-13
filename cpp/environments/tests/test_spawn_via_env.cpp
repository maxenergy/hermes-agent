#include "hermes/environments/spawn_via_env.hpp"
#include "hermes/environments/base.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace he = hermes::environments;

namespace {

// A fake environment that records each execute() invocation and plays
// back scripted responses in order.
class FakeEnvironment : public he::BaseEnvironment {
public:
    struct Invocation {
        std::string cmd;
        he::ExecuteOptions opts;
    };
    struct Script {
        int exit_code = 0;
        std::string stdout_text;
        std::string stderr_text;
    };

    std::string name() const override { return "fake"; }

    he::CompletedProcess execute(const std::string& cmd,
                                 const he::ExecuteOptions& opts) override {
        invocations.push_back({cmd, opts});
        he::CompletedProcess cp;
        if (idx_ < scripted.size()) {
            const auto& s = scripted[idx_++];
            cp.exit_code = s.exit_code;
            cp.stdout_text = s.stdout_text;
            cp.stderr_text = s.stderr_text;
        }
        return cp;
    }

    std::vector<Invocation> invocations;
    std::vector<Script> scripted;

private:
    std::size_t idx_ = 0;
};

}  // namespace

TEST(SpawnViaEnv, BootstrapScriptMentionsLogPidExitFiles) {
    FakeEnvironment env;
    env.scripted.push_back({0, "12345\n", ""});

    he::SpawnViaEnvOptions opts;
    opts.temp_dir = "/var/hermes";
    opts.handle_id = "abc";
    auto h = he::spawn_via_env(env, "sleep 5", opts);

    ASSERT_EQ(env.invocations.size(), 1u);
    const auto& cmd = env.invocations[0].cmd;
    EXPECT_NE(cmd.find("/var/hermes/hermes_bg_abc.log"), std::string::npos);
    EXPECT_NE(cmd.find("/var/hermes/hermes_bg_abc.pid"), std::string::npos);
    EXPECT_NE(cmd.find("/var/hermes/hermes_bg_abc.exit"), std::string::npos);
    EXPECT_NE(cmd.find("nohup"), std::string::npos);
    EXPECT_NE(cmd.find("sleep 5"), std::string::npos);

    ASSERT_TRUE(h.pid.has_value());
    EXPECT_EQ(*h.pid, 12345);
    EXPECT_EQ(h.handle_id, "abc");
    EXPECT_EQ(h.log_path, "/var/hermes/hermes_bg_abc.log");
}

TEST(SpawnViaEnv, ParsesPidFromMixedOutput) {
    FakeEnvironment env;
    env.scripted.push_back({0,
                            "some startup noise\n"
                            "login banner\n"
                            "987\n",
                            ""});
    auto h = he::spawn_via_env(env, "true");
    ASSERT_TRUE(h.pid.has_value());
    EXPECT_EQ(*h.pid, 987);
}

TEST(SpawnViaEnv, BackendFailurePopulatesError) {
    FakeEnvironment env;
    env.scripted.push_back({1, "", "bash: command not found"});
    auto h = he::spawn_via_env(env, "no-such-cmd");
    EXPECT_FALSE(h.pid.has_value());
    EXPECT_NE(h.error.find("not found"), std::string::npos);
}

TEST(SpawnViaEnv, PollReturnsNulloptWhenStillRunning) {
    FakeEnvironment env;
    env.scripted.push_back({0, "99\n", ""});
    env.scripted.push_back({0, "__HERMES_RUNNING__\n", ""});
    auto h = he::spawn_via_env(env, "true");
    auto rc = he::poll_background(env, h);
    EXPECT_FALSE(rc.has_value());
}

TEST(SpawnViaEnv, PollReturnsExitCodeOnCompletion) {
    FakeEnvironment env;
    env.scripted.push_back({0, "99\n", ""});
    env.scripted.push_back({0, "42\n", ""});
    auto h = he::spawn_via_env(env, "true");
    auto rc = he::poll_background(env, h);
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(*rc, 42);
}

TEST(SpawnViaEnv, ReadLogUsesTail) {
    FakeEnvironment env;
    env.scripted.push_back({0, "11\n", ""});
    env.scripted.push_back({0, "hello world\n", ""});
    auto h = he::spawn_via_env(env, "echo hello");
    auto log = he::read_background_log(env, h, 4096);
    EXPECT_EQ(log, "hello world\n");
    // Second invocation must be a tail -c.
    EXPECT_NE(env.invocations[1].cmd.find("tail -c 4096"),
              std::string::npos);
    EXPECT_NE(env.invocations[1].cmd.find("hermes_bg_"),
              std::string::npos);
}

TEST(SpawnViaEnv, KillSendsTermByDefault) {
    FakeEnvironment env;
    env.scripted.push_back({0, "12345\n", ""});
    env.scripted.push_back({0, "", ""});
    auto h = he::spawn_via_env(env, "sleep");
    ASSERT_TRUE(h.pid.has_value());
    EXPECT_TRUE(he::kill_background(env, h));
    const auto& cmd = env.invocations[1].cmd;
    EXPECT_NE(cmd.find("kill -TERM 12345"), std::string::npos);
}

TEST(SpawnViaEnv, KillWithForceSendsKill) {
    FakeEnvironment env;
    env.scripted.push_back({0, "55\n", ""});
    env.scripted.push_back({0, "", ""});
    auto h = he::spawn_via_env(env, "sleep");
    EXPECT_TRUE(he::kill_background(env, h, true));
    EXPECT_NE(env.invocations[1].cmd.find("kill -KILL 55"),
              std::string::npos);
}

TEST(SpawnViaEnv, KillWithoutPidFails) {
    FakeEnvironment env;
    env.scripted.push_back({1, "", "fail"});  // spawn fails → no pid
    auto h = he::spawn_via_env(env, "true");
    EXPECT_FALSE(he::kill_background(env, h));
}

TEST(SpawnViaEnv, HandleIdGeneratedWhenEmpty) {
    FakeEnvironment env;
    env.scripted.push_back({0, "7\n", ""});
    auto h = he::spawn_via_env(env, "true");
    EXPECT_FALSE(h.handle_id.empty());
    EXPECT_NE(h.handle_id.find("hbg_"), std::string::npos);
}
