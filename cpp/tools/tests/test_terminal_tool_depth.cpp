// Tests for hermes/tools/terminal_tool_depth.hpp — pure helpers mirroring
// decisions in tools/terminal_tool.py that previously lacked C++ coverage.
#include "hermes/tools/terminal_tool_depth.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace hermes::tools::terminal::depth;

TEST(TerminalDepthPreview, UnderLimit) {
    EXPECT_EQ(safe_command_preview("ls -la", 200), "ls -la");
}

TEST(TerminalDepthPreview, NewlinesReplaced) {
    EXPECT_EQ(safe_command_preview("foo\nbar\rbaz"), "foo bar baz");
}

TEST(TerminalDepthPreview, Truncated) {
    std::string cmd(500, 'x');
    auto out = safe_command_preview(cmd, 100);
    EXPECT_EQ(out.size(), 100u);
    EXPECT_EQ(out.substr(97), "...");
}

TEST(TerminalDepthPreview, TinyLimit) {
    EXPECT_EQ(safe_command_preview("abcdef", 2), "..");
}

TEST(TerminalDepthEnvAssign, Basic) {
    EXPECT_TRUE(looks_like_env_assignment("FOO=bar"));
    EXPECT_TRUE(looks_like_env_assignment("_X=y"));
    EXPECT_TRUE(looks_like_env_assignment("A1=value"));
}

TEST(TerminalDepthEnvAssign, Rejects) {
    EXPECT_FALSE(looks_like_env_assignment("=bar"));
    EXPECT_FALSE(looks_like_env_assignment("9FOO=bar"));
    EXPECT_FALSE(looks_like_env_assignment("no-equals"));
    EXPECT_FALSE(looks_like_env_assignment(""));
    EXPECT_FALSE(looks_like_env_assignment("FOO-BAR=x"));
}

TEST(TerminalDepthToken, PlainWord) {
    auto [tok, end] = read_shell_token("ls -la", 0);
    EXPECT_EQ(tok, "ls");
    EXPECT_EQ(end, 2u);
}

TEST(TerminalDepthToken, SingleQuoted) {
    auto [tok, end] = read_shell_token("'a b c' rest", 0);
    EXPECT_EQ(tok, "'a b c'");
    EXPECT_EQ(end, 7u);
}

TEST(TerminalDepthToken, DoubleQuotedWithEscape) {
    const std::string cmd = R"("a \"b\" c" rest)";
    auto [tok, end] = read_shell_token(cmd, 0);
    EXPECT_EQ(tok, R"("a \"b\" c")");
}

TEST(TerminalDepthToken, BackslashEscape) {
    auto [tok, end] = read_shell_token(R"(a\ b c)", 0);
    EXPECT_EQ(tok, R"(a\ b)");
}

TEST(TerminalDepthToken, StopsOnOperator) {
    auto [tok, end] = read_shell_token("foo;bar", 0);
    EXPECT_EQ(tok, "foo");
}

TEST(TerminalDepthSudo, RewritesBareSudo) {
    auto out = rewrite_real_sudo_invocations("sudo apt update");
    EXPECT_TRUE(out.found_sudo);
    EXPECT_NE(out.command.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalDepthSudo, NoSudoUnchanged) {
    auto out = rewrite_real_sudo_invocations("ls -la");
    EXPECT_FALSE(out.found_sudo);
    EXPECT_EQ(out.command, "ls -la");
}

TEST(TerminalDepthSudo, SkipsQuotedMention) {
    auto out = rewrite_real_sudo_invocations("echo \"sudo is tricky\"");
    EXPECT_FALSE(out.found_sudo);
    EXPECT_EQ(out.command, "echo \"sudo is tricky\"");
}

TEST(TerminalDepthSudo, RewritesAfterAmpAmp) {
    auto out = rewrite_real_sudo_invocations("ls && sudo rm -rf foo");
    EXPECT_TRUE(out.found_sudo);
    EXPECT_NE(out.command.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalDepthSudo, RewritesAfterSemicolon) {
    auto out = rewrite_real_sudo_invocations("true; sudo reboot");
    EXPECT_TRUE(out.found_sudo);
}

TEST(TerminalDepthSudo, RewritesAfterPipe) {
    auto out = rewrite_real_sudo_invocations("echo hi | sudo tee /etc/motd");
    EXPECT_TRUE(out.found_sudo);
}

TEST(TerminalDepthSudo, KeepsEnvPrefix) {
    auto out = rewrite_real_sudo_invocations("FOO=bar sudo cmd");
    EXPECT_TRUE(out.found_sudo);
    EXPECT_NE(out.command.find("FOO=bar"), std::string::npos);
    EXPECT_NE(out.command.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalDepthSudo, IgnoresInsideComment) {
    auto out =
        rewrite_real_sudo_invocations("# sudo is dangerous\nls -la");
    EXPECT_FALSE(out.found_sudo);
}

TEST(TerminalDepthExitCode, ZeroReturnsNullopt) {
    EXPECT_FALSE(interpret_exit_code("grep foo file", 0).has_value());
}

TEST(TerminalDepthExitCode, GrepNoMatch) {
    auto msg = interpret_exit_code("grep missing file", 1);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("No matches"), std::string::npos);
}

TEST(TerminalDepthExitCode, RipgrepNoMatch) {
    auto msg = interpret_exit_code("rg pattern", 1);
    ASSERT_TRUE(msg.has_value());
}

TEST(TerminalDepthExitCode, DiffFilesDiffer) {
    auto msg = interpret_exit_code("diff a b", 1);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("differ"), std::string::npos);
}

TEST(TerminalDepthExitCode, CurlDnsFail) {
    auto msg = interpret_exit_code("curl https://bad/", 6);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("resolve"), std::string::npos);
}

TEST(TerminalDepthExitCode, CurlUnknownExitCode) {
    EXPECT_FALSE(interpret_exit_code("curl https://x/", 99).has_value());
}

TEST(TerminalDepthExitCode, PipelineLastWins) {
    auto msg = interpret_exit_code("cat file | grep pattern", 1);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("No matches"), std::string::npos);
}

TEST(TerminalDepthExitCode, UnknownCommand) {
    EXPECT_FALSE(interpret_exit_code("make build", 2).has_value());
}

TEST(TerminalDepthExitCode, BasePathStripped) {
    auto msg = interpret_exit_code("/usr/bin/grep x y", 1);
    ASSERT_TRUE(msg.has_value());
}

TEST(TerminalDepthExitCode, GitExitOne) {
    auto msg = interpret_exit_code("git diff", 1);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("git diff"), std::string::npos);
}

TEST(TerminalDepthExitCode, EnvAssignmentSkipped) {
    auto msg = interpret_exit_code("LANG=C grep foo file", 1);
    ASSERT_TRUE(msg.has_value());
    EXPECT_NE(msg->find("No matches"), std::string::npos);
}

TEST(TerminalDepthPipe, GhAuthLoginWithToken) {
    EXPECT_TRUE(
        command_requires_pipe_stdin("gh auth login --with-token"));
    EXPECT_TRUE(command_requires_pipe_stdin("  GH  AUTH  LOGIN  --with-token "));
}

TEST(TerminalDepthPipe, OtherGhCmd) {
    EXPECT_FALSE(command_requires_pipe_stdin("gh auth status"));
    EXPECT_FALSE(command_requires_pipe_stdin("gh auth login"));
    EXPECT_FALSE(command_requires_pipe_stdin("ls -la"));
}

TEST(TerminalDepthDisk, BelowThreshold) {
    DiskWarningInput in;
    in.total_gb = 400;
    in.threshold_gb = 500;
    auto out = evaluate_disk_warning(in);
    EXPECT_FALSE(out.should_warn);
}

TEST(TerminalDepthDisk, AboveThreshold) {
    DiskWarningInput in;
    in.total_gb = 600;
    in.threshold_gb = 500;
    auto out = evaluate_disk_warning(in);
    EXPECT_TRUE(out.should_warn);
    EXPECT_NE(out.message.find("600.0"), std::string::npos);
    EXPECT_NE(out.message.find("500.0"), std::string::npos);
}

TEST(TerminalDepthDisk, AlreadyWarnedSuppressed) {
    DiskWarningInput in;
    in.total_gb = 9999;
    in.threshold_gb = 1;
    in.already_warned_today = true;
    auto out = evaluate_disk_warning(in);
    EXPECT_FALSE(out.should_warn);
}

TEST(TerminalDepthClamp, ClampAboveCap) {
    EXPECT_EQ(clamp_foreground_timeout(9999, 300, 600), 600);
}

TEST(TerminalDepthClamp, ZeroFallsBackToDefault) {
    EXPECT_EQ(clamp_foreground_timeout(0, 300, 600), 300);
}

TEST(TerminalDepthClamp, NegativeFallsBack) {
    EXPECT_EQ(clamp_foreground_timeout(-1, 300, 600), 300);
}

TEST(TerminalDepthClamp, DefaultAboveCap) {
    EXPECT_EQ(clamp_foreground_timeout(0, 9999, 600), 600);
}

TEST(TerminalDepthEnvParse, IntOk) {
    EXPECT_EQ(parse_env_int("42", 7), 42);
}

TEST(TerminalDepthEnvParse, IntBad) {
    EXPECT_EQ(parse_env_int("abc", 7), 7);
    EXPECT_EQ(parse_env_int("42abc", 7), 7);
    EXPECT_EQ(parse_env_int("", 7), 7);
}

TEST(TerminalDepthEnvParse, DoubleOk) {
    EXPECT_DOUBLE_EQ(parse_env_double("3.5", 1.0), 3.5);
}

TEST(TerminalDepthEnvParse, DoubleBad) {
    EXPECT_DOUBLE_EQ(parse_env_double("nope", 1.0), 1.0);
    EXPECT_DOUBLE_EQ(parse_env_double("", 1.0), 1.0);
}

TEST(TerminalDepthMask, TokenFlag) {
    auto m = mask_secret_args("curl --token=abcdef https://x/");
    EXPECT_GE(m.replaced, 1u);
    EXPECT_EQ(m.redacted.find("abcdef"), std::string::npos);
}

TEST(TerminalDepthMask, ApiKeyFlag) {
    auto m = mask_secret_args("foo --api-key SECRET_VAL bar");
    EXPECT_GE(m.replaced, 1u);
    EXPECT_EQ(m.redacted.find("SECRET_VAL"), std::string::npos);
}

TEST(TerminalDepthMask, EnvVarAssignment) {
    auto m = mask_secret_args("GITHUB_TOKEN=ghp_abc gh repo list");
    EXPECT_GE(m.replaced, 1u);
    EXPECT_EQ(m.redacted.find("ghp_abc"), std::string::npos);
}

TEST(TerminalDepthMask, NoSecrets) {
    auto m = mask_secret_args("ls -la");
    EXPECT_EQ(m.replaced, 0u);
    EXPECT_EQ(m.redacted, "ls -la");
}

TEST(TerminalDepthPipeline, LastSegment) {
    EXPECT_EQ(last_pipeline_segment("a | b"), "b");
    EXPECT_EQ(last_pipeline_segment("a && b"), "b");
    EXPECT_EQ(last_pipeline_segment("a || b ; c"), "c");
    EXPECT_EQ(last_pipeline_segment("only"), "only");
}

TEST(TerminalDepthPipeline, ExtractBaseCommand) {
    EXPECT_EQ(extract_base_command("grep foo"), "grep");
    EXPECT_EQ(extract_base_command("FOO=bar ls -la"), "ls");
    EXPECT_EQ(extract_base_command("/usr/bin/rg pat"), "rg");
    EXPECT_EQ(extract_base_command(""), "");
}
