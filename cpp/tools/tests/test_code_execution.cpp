#include "hermes/tools/code_execution_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace hermes::tools;
using namespace hermes::tools::code_execution;
using json = nlohmann::json;

namespace {

class CodeExecutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
        code_execution::register_code_execution_tools(ToolRegistry::instance());
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
    auto r = json::parse(dispatch("execute_code",
        {{"language", "bash"}, {"code", "sleep 10"}, {"timeout", 1}}));
    EXPECT_TRUE(r["timed_out"].get<bool>());
    EXPECT_NE(r["exit_code"].get<int>(), 0);
}

TEST_F(CodeExecutionTest, CodeWithErrorNonZeroExit) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "bash"}, {"code", "exit 42"}}));
    EXPECT_EQ(r["exit_code"].get<int>(), 42);
}

TEST_F(CodeExecutionTest, PythonErrorNonZeroExit) {
    auto r = json::parse(dispatch("execute_code",
        {{"language", "python"}, {"code", "raise ValueError('boom')"}}));
    EXPECT_NE(r["exit_code"].get<int>(), 0);
    EXPECT_NE(r["stderr"].get<std::string>().find("ValueError"),
              std::string::npos);
}

TEST_F(CodeExecutionTest, UnsupportedLanguageErrors) {
    auto r = json::parse(dispatch(
        "execute_code",
        {{"language", "rust"}, {"code", "fn main(){}"}}));
    EXPECT_TRUE(r.contains("error"));
}

TEST_F(CodeExecutionTest, MissingLanguageErrors) {
    auto r = json::parse(dispatch("execute_code",
                                  {{"code", "print(1)"}}));
    EXPECT_TRUE(r.contains("error"));
}

TEST_F(CodeExecutionTest, MissingCodeErrors) {
    auto r = json::parse(dispatch("execute_code",
                                  {{"language", "python"}}));
    EXPECT_TRUE(r.contains("error"));
}

TEST_F(CodeExecutionTest, TimeoutEchoInResult) {
    auto r = json::parse(dispatch(
        "execute_code",
        {{"language", "bash"}, {"code", "echo hi"}, {"timeout", 5}}));
    EXPECT_EQ(r["timeout"].get<int>(), 5);
    EXPECT_EQ(r["language"].get<std::string>(), "bash");
}

TEST_F(CodeExecutionTest, TimeoutClampedToMax) {
    auto r = json::parse(dispatch(
        "execute_code",
        {{"language", "bash"}, {"code", "echo hi"}, {"timeout", 10000}}));
    EXPECT_LE(r["timeout"].get<int>(), kMaxTimeoutSeconds);
}

// ── helper coverage ──────────────────────────────────────────────────

TEST(CodeExecUtil, ShellQuotePlain) {
    EXPECT_EQ(shell_quote("hello"), "hello");
    EXPECT_EQ(shell_quote("abc123"), "abc123");
    EXPECT_EQ(shell_quote("/tmp/file-1.txt"), "/tmp/file-1.txt");
}

TEST(CodeExecUtil, ShellQuoteEmpty) {
    EXPECT_EQ(shell_quote(""), "''");
}

TEST(CodeExecUtil, ShellQuoteSpecial) {
    EXPECT_EQ(shell_quote("a b"), "'a b'");
    EXPECT_EQ(shell_quote("he said"), "'he said'");
    // Embedded single quote.
    EXPECT_EQ(shell_quote("it's"), "'it'\\''s'");
}

TEST(CodeExecUtil, TruncateOutputShort) {
    std::string s(100, 'x');
    EXPECT_EQ(truncate_output(s), s);
}

TEST(CodeExecUtil, TruncateOutputLong) {
    std::string s(100 * 1024, 'x');
    auto t = truncate_output(s);
    EXPECT_LT(t.size(), s.size());
    EXPECT_NE(t.find("truncated"), std::string::npos);
}

TEST(CodeExecUtil, ClampTimeout) {
    EXPECT_EQ(clamp_timeout(0), kMinTimeoutSeconds);
    EXPECT_EQ(clamp_timeout(-5), kMinTimeoutSeconds);
    EXPECT_EQ(clamp_timeout(100000), kMaxTimeoutSeconds);
    EXPECT_EQ(clamp_timeout(60), 60);
}

TEST(CodeExecUtil, GenerateUuidIsHex) {
    auto u = generate_uuid();
    EXPECT_EQ(u.size(), 32u);
    for (char c : u) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    auto u2 = generate_uuid();
    EXPECT_NE(u, u2);
}

TEST(CodeExecUtil, SandboxAllowedTools) {
    const auto& s = sandbox_allowed_tools();
    EXPECT_TRUE(s.count("web_search"));
    EXPECT_TRUE(s.count("read_file"));
    EXPECT_TRUE(s.count("terminal"));
    EXPECT_FALSE(s.count("not_a_real_tool"));
}

TEST(CodeExecUtil, GenerateHermesToolsModuleEmpty) {
    auto m = generate_hermes_tools_module({});
    // Header is present but no stubs were emitted.
    EXPECT_NE(m.find("Auto-generated"), std::string::npos);
    EXPECT_EQ(m.find("def web_search"), std::string::npos);
}

TEST(CodeExecUtil, GenerateHermesToolsModuleFiltersUnknown) {
    auto m = generate_hermes_tools_module({"web_search", "not_a_tool"});
    EXPECT_NE(m.find("def web_search"), std::string::npos);
    EXPECT_EQ(m.find("not_a_tool"), std::string::npos);
}

TEST(CodeExecUtil, GenerateHermesToolsModuleUdsVsFile) {
    auto uds = generate_hermes_tools_module({"read_file"}, "uds");
    auto file = generate_hermes_tools_module({"read_file"}, "file");
    EXPECT_NE(uds, file);
    EXPECT_NE(uds.find("UDS transport"), std::string::npos);
    EXPECT_NE(file.find("file transport"), std::string::npos);
}

TEST(CodeExecUtil, BuildExecuteCodeSchema) {
    auto s = build_execute_code_schema();
    EXPECT_TRUE(s.contains("properties"));
    EXPECT_TRUE(s["properties"].contains("language"));
    EXPECT_TRUE(s["properties"]["language"].contains("enum"));
}

TEST(CodeExecUtil, BuildExecuteCodeSchemaWithTools) {
    auto s = build_execute_code_schema({"web_search", "read_file"});
    ASSERT_TRUE(s.contains("description"));
    EXPECT_NE(s["description"].get<std::string>().find("web_search"),
              std::string::npos);
    EXPECT_NE(s["description"].get<std::string>().find("read_file"),
              std::string::npos);
}

}  // namespace
