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
