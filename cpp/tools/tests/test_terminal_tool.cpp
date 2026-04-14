#include "hermes/tools/terminal_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

using json = nlohmann::json;
using namespace hermes::tools;

namespace {

class TerminalToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_terminal_tools();
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
    }

    std::string dispatch(const std::string& name, const json& args) {
        ToolContext ctx;
        ctx.cwd = "/tmp";
        return ToolRegistry::instance().dispatch(name, args, ctx);
    }
};

// -- terminal foreground tests ---------------------------------------------

// Factory routing: when a TerminalEnvFactory is installed the factory
// is invoked with the environment name from ctx.extra and the returned
// env is used to execute commands.  Observable proxy that forwards execute() to a shared sink so the test
// can inspect the command and control the return value.  A fresh
// proxy is created on each factory invocation (the unique_ptr deletes
// the proxy, not the sink).
struct StubSink {
    std::string last_cmd;
    int exit_code = 99;
    std::string stdout_text = "stubbed";
};
class ProxyEnv : public hermes::environments::BaseEnvironment {
public:
    explicit ProxyEnv(StubSink* s) : sink_(s) {}
    std::string name() const override { return "proxy"; }
    hermes::environments::CompletedProcess execute(
        const std::string& cmd,
        const hermes::environments::ExecuteOptions& /*opts*/) override {
        sink_->last_cmd = cmd;
        hermes::environments::CompletedProcess p;
        p.exit_code = sink_->exit_code;
        p.stdout_text = sink_->stdout_text;
        return p;
    }
private:
    StubSink* sink_;
};

TEST_F(TerminalToolTest, FactoryOverrideRoutesExecution) {
    StubSink sink;
    std::string seen_name;
    set_terminal_env_factory([&](const std::string& name)
        -> std::unique_ptr<hermes::environments::BaseEnvironment> {
        seen_name = name;
        return std::make_unique<ProxyEnv>(&sink);
    });

    ToolContext ctx;
    ctx.extra = nlohmann::json{{"environment", "docker"}};
    auto out = ToolRegistry::instance().dispatch("terminal",
        {{"command", "echo routed"}}, ctx);
    set_terminal_env_factory({});  // reset

    auto result = json::parse(out);
    EXPECT_EQ(result["exit_code"], 99);
    EXPECT_EQ(result["stdout"], "stubbed");
    EXPECT_EQ(seen_name, "docker");
    EXPECT_EQ(sink.last_cmd, "echo routed");
}

TEST_F(TerminalToolTest, FactoryFallsBackToLocalWhenUnset) {
    // No factory installed — execution goes to LocalEnvironment.
    set_terminal_env_factory({});
    auto result = json::parse(dispatch("terminal", {{"command", "echo fallback"}}));
    EXPECT_EQ(result["exit_code"], 0);
    EXPECT_NE(result["stdout"].get<std::string>().find("fallback"),
              std::string::npos);
}

TEST_F(TerminalToolTest, ForegroundEchoHello) {
    auto result = json::parse(dispatch("terminal", {{"command", "echo hello"}}));
    EXPECT_EQ(result["exit_code"], 0);
    EXPECT_NE(result["stdout"].get<std::string>().find("hello"), std::string::npos);
    EXPECT_FALSE(result["timed_out"].get<bool>());
}

TEST_F(TerminalToolTest, ForegroundExitCode) {
    auto result = json::parse(dispatch("terminal", {{"command", "exit 42"}}));
    EXPECT_EQ(result["exit_code"], 42);
}

TEST_F(TerminalToolTest, ForegroundTimeout) {
    auto result = json::parse(
        dispatch("terminal", {{"command", "sleep 30"}, {"timeout", 2}}));
    EXPECT_TRUE(result["timed_out"].get<bool>());
}

// -- terminal background tests ---------------------------------------------

TEST_F(TerminalToolTest, BackgroundSpawnsAndReturnsProcessId) {
    auto result = json::parse(
        dispatch("terminal",
                 {{"command", "sleep 1"}, {"background", true}}));
    EXPECT_TRUE(result["started"].get<bool>());
    EXPECT_TRUE(result.contains("process_id"));
    EXPECT_FALSE(result["process_id"].get<std::string>().empty());
}

TEST_F(TerminalToolTest, ProcessStatusOfRunningProcess) {
    auto bg = json::parse(
        dispatch("terminal",
                 {{"command", "sleep 10"}, {"background", true}}));
    std::string pid = bg["process_id"];

    // Give the thread a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto status = json::parse(
        dispatch("process", {{"process_id", pid}, {"action", "status"}}));
    EXPECT_EQ(status["state"], "running");

    // Clean up — kill the process.
    dispatch("process", {{"process_id", pid}, {"action", "kill"}});
}

TEST_F(TerminalToolTest, ProcessKill) {
    auto bg = json::parse(
        dispatch("terminal",
                 {{"command", "sleep 60"}, {"background", true}}));
    std::string pid = bg["process_id"];

    auto kill_result = json::parse(
        dispatch("process", {{"process_id", pid}, {"action", "kill"}}));
    EXPECT_TRUE(kill_result["killed"].get<bool>());

    // After kill, status should be killed.
    auto status = json::parse(
        dispatch("process", {{"process_id", pid}, {"action", "status"}}));
    EXPECT_EQ(status["state"], "killed");
}

// ── Helper-function coverage ─────────────────────────────────────────

TEST(TerminalUtil, SafeCommandPreviewShort) {
    EXPECT_EQ(terminal::safe_command_preview("echo hi"), "echo hi");
    EXPECT_EQ(terminal::safe_command_preview(""), "<empty>");
}

TEST(TerminalUtil, SafeCommandPreviewTruncates) {
    std::string long_cmd(500, 'x');
    auto s = terminal::safe_command_preview(long_cmd, 100);
    EXPECT_LE(s.size(), 105u);
    EXPECT_NE(s.find("..."), std::string::npos);
}

TEST(TerminalUtil, LooksLikeEnvAssignment) {
    EXPECT_TRUE(terminal::looks_like_env_assignment("FOO=bar"));
    EXPECT_TRUE(terminal::looks_like_env_assignment("_X=1"));
    EXPECT_TRUE(terminal::looks_like_env_assignment("PATH=/usr/bin"));
    EXPECT_FALSE(terminal::looks_like_env_assignment("=bar"));
    EXPECT_FALSE(terminal::looks_like_env_assignment("123=bar"));
    EXPECT_FALSE(terminal::looks_like_env_assignment("echo hi"));
    EXPECT_FALSE(terminal::looks_like_env_assignment(""));
    EXPECT_FALSE(terminal::looks_like_env_assignment("FOO"));
}

TEST(TerminalUtil, ReadShellTokenPlain) {
    auto [tok, end] = terminal::read_shell_token("echo hello", 0);
    EXPECT_EQ(tok, "echo");
    EXPECT_EQ(end, 4u);
}

TEST(TerminalUtil, ReadShellTokenQuoted) {
    auto [tok, end] = terminal::read_shell_token("'foo bar' baz", 0);
    EXPECT_EQ(tok, "'foo bar'");
    EXPECT_EQ(end, 9u);
}

TEST(TerminalUtil, ReadShellTokenDoubleQuoted) {
    auto [tok, end] = terminal::read_shell_token("\"a\\\"b\" c", 0);
    EXPECT_EQ(tok, "\"a\\\"b\"");
    EXPECT_EQ(end, 6u);
}

TEST(TerminalUtil, SudoRewriteBare) {
    auto [cmd, found] =
        terminal::rewrite_real_sudo_invocations("sudo apt-get update");
    EXPECT_TRUE(found);
    EXPECT_NE(cmd.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalUtil, SudoRewriteChained) {
    auto [cmd, found] =
        terminal::rewrite_real_sudo_invocations(
            "echo x && sudo apt-get update");
    EXPECT_TRUE(found);
    EXPECT_NE(cmd.find("sudo -S"), std::string::npos);
}

TEST(TerminalUtil, SudoMentionInStringNotRewritten) {
    auto [cmd, found] = terminal::rewrite_real_sudo_invocations(
        "echo 'please sudo this'");
    EXPECT_FALSE(found);
    EXPECT_EQ(cmd, "echo 'please sudo this'");
}

TEST(TerminalUtil, SudoNotRewrittenInComment) {
    auto [cmd, found] = terminal::rewrite_real_sudo_invocations(
        "# sudo is needed\necho hi");
    EXPECT_FALSE(found);
}

TEST(TerminalUtil, InterpretExitCodeGrepNoMatch) {
    auto note = terminal::interpret_exit_code("grep foo /tmp/file", 1);
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("No matches"), std::string::npos);
}

TEST(TerminalUtil, InterpretExitCodeZero) {
    EXPECT_FALSE(terminal::interpret_exit_code("grep x y", 0).has_value());
}

TEST(TerminalUtil, InterpretExitCodeDiffFilesDiffer) {
    auto note = terminal::interpret_exit_code("diff a b", 1);
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("differ"), std::string::npos);
}

TEST(TerminalUtil, InterpretExitCodeCurlCodes) {
    auto n6 = terminal::interpret_exit_code("curl https://x", 6);
    ASSERT_TRUE(n6.has_value());
    EXPECT_NE(n6->find("resolve"), std::string::npos);
    auto n28 = terminal::interpret_exit_code("curl https://x", 28);
    ASSERT_TRUE(n28.has_value());
    EXPECT_NE(n28->find("timed out"), std::string::npos);
}

TEST(TerminalUtil, InterpretExitCodePipelineUsesLast) {
    auto note = terminal::interpret_exit_code("echo x | grep foo", 1);
    ASSERT_TRUE(note.has_value());
    EXPECT_NE(note->find("No matches"), std::string::npos);
}

TEST(TerminalUtil, InterpretExitCodeUnknownCommandReturnsNone) {
    EXPECT_FALSE(terminal::interpret_exit_code("uuid-regen", 1).has_value());
}

TEST(TerminalUtil, CommandRequiresPipeStdin) {
    EXPECT_TRUE(terminal::command_requires_pipe_stdin(
        "gh auth login --with-token"));
    EXPECT_TRUE(terminal::command_requires_pipe_stdin(
        "gh   auth   login   --with-token"));
    EXPECT_FALSE(terminal::command_requires_pipe_stdin("gh auth login"));
    EXPECT_FALSE(terminal::command_requires_pipe_stdin("echo hi"));
}

TEST(TerminalUtil, ValidateWorkdirRejectsRelative) {
    auto r = terminal::validate_workdir("relative/path");
    EXPECT_FALSE(r.error.empty());
}

TEST(TerminalUtil, ValidateWorkdirAcceptsTmp) {
    auto r = terminal::validate_workdir("/tmp");
    EXPECT_TRUE(r.error.empty());
    EXPECT_FALSE(r.path.empty());
}

TEST(TerminalUtil, ValidateWorkdirMissingErrors) {
    auto r = terminal::validate_workdir("/this/does/not/exist/xyz");
    EXPECT_FALSE(r.error.empty());
}

TEST(TerminalUtil, ClampTimeout) {
    EXPECT_EQ(terminal::clamp_timeout(0), 1);
    EXPECT_EQ(terminal::clamp_timeout(-10), 1);
    EXPECT_EQ(terminal::clamp_timeout(10000), 3600);
    EXPECT_EQ(terminal::clamp_timeout(120), 120);
}

}  // namespace
