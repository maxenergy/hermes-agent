#include "hermes/tools/code_execution_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using namespace hermes::tools;
using json = nlohmann::json;

namespace {

class CodeExecutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        register_code_execution_tools(ToolRegistry::instance());
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
    }

    std::string dispatch(const std::string& tool, const json& args) {
        return ToolRegistry::instance().dispatch(tool, args, ctx_);
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(CodeExecutionTest, PythonHelloWorld) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "python"}, {"code", "print('hello world')"}}));
    EXPECT_EQ(r["exit_code"].get<int>(), 0);
    EXPECT_FALSE(r["timed_out"].get<bool>());
    EXPECT_NE(r["stdout"].get<std::string>().find("hello world"),
              std::string::npos);
}

TEST_F(CodeExecutionTest, BashEcho) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "bash"}, {"code", "echo 'bash works'"}}));
    EXPECT_EQ(r["exit_code"].get<int>(), 0);
    EXPECT_FALSE(r["timed_out"].get<bool>());
    EXPECT_NE(r["stdout"].get<std::string>().find("bash works"),
              std::string::npos);
}

TEST_F(CodeExecutionTest, TimeoutShortCommand) {
    // A very short timeout on a sleep command should time out.
    auto r = json::parse(dispatch("execute_code",
        {{"language", "bash"}, {"code", "sleep 10"}, {"timeout", 1}}));
    EXPECT_TRUE(r["timed_out"].get<bool>());
    EXPECT_NE(r["exit_code"].get<int>(), 0);
}

TEST_F(CodeExecutionTest, CodeWithErrorNonZeroExit) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "bash"}, {"code", "exit 42"}}));
    EXPECT_EQ(r["exit_code"].get<int>(), 42);
    EXPECT_FALSE(r["timed_out"].get<bool>());
}

TEST_F(CodeExecutionTest, PythonErrorNonZeroExit) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "python"}, {"code", "raise ValueError('boom')"}}));
    EXPECT_NE(r["exit_code"].get<int>(), 0);
    EXPECT_NE(r["stderr"].get<std::string>().find("ValueError"),
              std::string::npos);
}

}  // namespace
