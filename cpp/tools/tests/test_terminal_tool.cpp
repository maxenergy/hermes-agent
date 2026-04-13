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

}  // namespace
