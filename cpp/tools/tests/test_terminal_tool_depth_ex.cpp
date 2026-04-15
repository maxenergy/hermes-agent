// Tests for hermes/tools/terminal_tool_depth_ex.hpp.
#include "hermes/tools/terminal_tool_depth_ex.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hermes::tools::terminal::depth_ex;

TEST(TerminalDepthExPreview, NoneValue) {
    EXPECT_EQ(safe_command_preview(std::nullopt), "<None>");
}

TEST(TerminalDepthExPreview, ShortString) {
    EXPECT_EQ(safe_command_preview(std::optional<std::string_view>{"echo"}),
              "echo");
}

TEST(TerminalDepthExPreview, TruncatesLongString) {
    std::string big(500, 'x');
    std::string_view view{big};
    auto out = safe_command_preview(std::optional<std::string_view>{view}, 50);
    EXPECT_EQ(out.size(), 50u);
}

TEST(TerminalDepthExToken, BarewordStopsAtSpace) {
    auto [tok, next] = read_shell_token("echo hello", 0);
    EXPECT_EQ(tok, "echo");
    EXPECT_EQ(next, 4u);
}

TEST(TerminalDepthExToken, SingleQuotedPreserved) {
    auto [tok, next] = read_shell_token("'hello world' rest", 0);
    EXPECT_EQ(tok, "'hello world'");
    EXPECT_EQ(next, 13u);
}

TEST(TerminalDepthExToken, DoubleQuotedWithEscape) {
    auto [tok, next] = read_shell_token(R"("she said \"hi\"")", 0);
    // Including the outer quotes, the token is the entire quoted span.
    EXPECT_GT(tok.size(), 5u);
    EXPECT_EQ(tok.front(), '"');
    EXPECT_EQ(tok.back(), '"');
    EXPECT_EQ(next, tok.size());
}

TEST(TerminalDepthExToken, StopsAtPipe) {
    auto [tok, next] = read_shell_token("cat|grep", 0);
    EXPECT_EQ(tok, "cat");
    EXPECT_EQ(next, 3u);
}

TEST(TerminalDepthExToken, BackslashEscape) {
    auto [tok, next] = read_shell_token(R"(foo\ bar rest)", 0);
    EXPECT_EQ(tok, R"(foo\ bar)");
    EXPECT_EQ(next, 8u);
}

TEST(TerminalDepthExEnvAssign, Positive) {
    EXPECT_TRUE(looks_like_env_assignment("FOO=bar"));
    EXPECT_TRUE(looks_like_env_assignment("_X=1"));
    EXPECT_TRUE(looks_like_env_assignment("A1_2=v"));
}

TEST(TerminalDepthExEnvAssign, Negative) {
    EXPECT_FALSE(looks_like_env_assignment("=bar"));
    EXPECT_FALSE(looks_like_env_assignment("FOO"));
    EXPECT_FALSE(looks_like_env_assignment("1FOO=bar"));
    EXPECT_FALSE(looks_like_env_assignment("FOO-BAR=x"));
    EXPECT_FALSE(looks_like_env_assignment(""));
}

TEST(TerminalDepthExSudo, RewritesLeadingSudo) {
    auto r = rewrite_real_sudo_invocations("sudo apt-get install curl");
    EXPECT_TRUE(r.rewrote);
    EXPECT_EQ(r.command.rfind("sudo -S -p ''", 0), 0u);
}

TEST(TerminalDepthExSudo, DoesNotRewriteInString) {
    auto r = rewrite_real_sudo_invocations("echo 'sudo ran'");
    EXPECT_FALSE(r.rewrote);
    EXPECT_EQ(r.command, "echo 'sudo ran'");
}

TEST(TerminalDepthExSudo, DoesNotRewriteInComment) {
    auto r = rewrite_real_sudo_invocations("# sudo is cool\necho hi");
    EXPECT_FALSE(r.rewrote);
}

TEST(TerminalDepthExSudo, RewritesAfterLogicalSeparator) {
    auto r = rewrite_real_sudo_invocations("cd /tmp && sudo ls");
    EXPECT_TRUE(r.rewrote);
    EXPECT_NE(r.command.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalDepthExSudo, SkipsLeadingEnvAssignments) {
    auto r = rewrite_real_sudo_invocations("FOO=bar sudo ls");
    EXPECT_TRUE(r.rewrote);
    EXPECT_NE(r.command.find("sudo -S -p ''"), std::string::npos);
}

TEST(TerminalDepthExSudo, PipeTriggersNewCommand) {
    auto r = rewrite_real_sudo_invocations("cat file | sudo tee out");
    EXPECT_TRUE(r.rewrote);
}

TEST(TerminalDepthExExitCode, ZeroReturnsEmpty) {
    EXPECT_EQ(interpret_exit_code("any", 0), "");
}

TEST(TerminalDepthExExitCode, GrepNoMatches) {
    EXPECT_NE(interpret_exit_code("grep foo bar", 1), "");
    EXPECT_NE(interpret_exit_code("rg foo", 1), "");
    EXPECT_NE(interpret_exit_code("/usr/bin/grep foo", 1), "");
}

TEST(TerminalDepthExExitCode, GrepRealError) {
    EXPECT_EQ(interpret_exit_code("grep foo", 2), "");
}

TEST(TerminalDepthExExitCode, DiffFilesDiffer) {
    EXPECT_NE(interpret_exit_code("diff a b", 1), "");
    EXPECT_NE(interpret_exit_code("colordiff a b", 1), "");
}

TEST(TerminalDepthExExitCode, Curl) {
    EXPECT_NE(interpret_exit_code("curl x", 6), "");
    EXPECT_NE(interpret_exit_code("curl x", 22), "");
    EXPECT_EQ(interpret_exit_code("curl x", 3), "");
}

TEST(TerminalDepthExExitCode, GitDiff) {
    EXPECT_NE(interpret_exit_code("git diff", 1), "");
}

TEST(TerminalDepthExExitCode, UsesLastSegment) {
    // Exit code comes from the last command in a pipeline.
    EXPECT_NE(interpret_exit_code("ls foo && grep bar baz", 1), "");
    EXPECT_NE(interpret_exit_code("echo hi | diff - /etc/hosts", 1), "");
}

TEST(TerminalDepthExExitCode, UnknownCommandEmpty) {
    EXPECT_EQ(interpret_exit_code("mytool --foo", 1), "");
}

TEST(TerminalDepthExSegments, LastCommandSegment) {
    EXPECT_EQ(last_command_segment("a && b"), "b");
    EXPECT_EQ(last_command_segment("a || b || c"), "c");
    EXPECT_EQ(last_command_segment("a | b"), "b");
    EXPECT_EQ(last_command_segment("a ; b"), "b");
    EXPECT_EQ(last_command_segment("single"), "single");
}

TEST(TerminalDepthExBaseCmd, SkipsEnvAssignments) {
    EXPECT_EQ(extract_base_command("FOO=bar grep hi"), "grep");
    EXPECT_EQ(extract_base_command("A=1 B=2 find ."), "find");
}

TEST(TerminalDepthExBaseCmd, StripsPath) {
    EXPECT_EQ(extract_base_command("/usr/bin/grep -i"), "grep");
}

TEST(TerminalDepthExBaseCmd, EmptyWhenNothing) {
    EXPECT_EQ(extract_base_command("  "), "");
    EXPECT_EQ(extract_base_command("FOO=bar"), "");
}

TEST(TerminalDepthExPipeStdin, MatchesGhAuth) {
    EXPECT_TRUE(command_requires_pipe_stdin("gh auth login --with-token"));
    EXPECT_TRUE(command_requires_pipe_stdin(
        "  gh   auth   login    --with-token  "));
    EXPECT_TRUE(command_requires_pipe_stdin(
        "GH AUTH LOGIN --with-token --hostname foo"));
}

TEST(TerminalDepthExPipeStdin, DoesNotMatchOthers) {
    EXPECT_FALSE(command_requires_pipe_stdin("gh auth login"));
    EXPECT_FALSE(command_requires_pipe_stdin("other command --with-token"));
    EXPECT_FALSE(command_requires_pipe_stdin(""));
}

TEST(TerminalDepthExWorkdir, RejectsEmpty) {
    EXPECT_NE(validate_workdir(""), "");
}

TEST(TerminalDepthExWorkdir, AcceptsSimple) {
    EXPECT_EQ(validate_workdir("/tmp"), "");
    EXPECT_EQ(validate_workdir("/home/user/dir with spaces"), "");
}

TEST(TerminalDepthExWorkdir, RejectsShellMeta) {
    EXPECT_NE(validate_workdir("/tmp/$HOME"), "");
    EXPECT_NE(validate_workdir("/tmp/`whoami`"), "");
    EXPECT_NE(validate_workdir("/tmp\nrm -rf /"), "");
}

TEST(TerminalDepthExEnvInt, ParseDefaults) {
    EXPECT_EQ(parse_env_int(std::nullopt, 99), 99);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{""}, 99), 99);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"  "}, 99), 99);
}

TEST(TerminalDepthExEnvInt, ParseValid) {
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"42"}, 0), 42);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"-7"}, 0), -7);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{" 100 "}, 0), 100);
}

TEST(TerminalDepthExEnvInt, RejectsInvalid) {
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"abc"}, 5), 5);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"1.5"}, 5), 5);
    EXPECT_EQ(parse_env_int(std::optional<std::string_view>{"--"}, 5), 5);
}

TEST(TerminalDepthExEnvBool, Truthy) {
    EXPECT_TRUE(parse_env_bool(std::optional<std::string_view>{"true"}, false));
    EXPECT_TRUE(parse_env_bool(std::optional<std::string_view>{"1"}, false));
    EXPECT_TRUE(parse_env_bool(std::optional<std::string_view>{"YES"}, false));
    EXPECT_TRUE(parse_env_bool(std::optional<std::string_view>{"on"}, false));
}

TEST(TerminalDepthExEnvBool, Falsy) {
    EXPECT_FALSE(parse_env_bool(std::optional<std::string_view>{"false"}, true));
    EXPECT_FALSE(parse_env_bool(std::optional<std::string_view>{"0"}, true));
    EXPECT_FALSE(parse_env_bool(std::optional<std::string_view>{"No"}, true));
    EXPECT_FALSE(parse_env_bool(std::optional<std::string_view>{"off"}, true));
}

TEST(TerminalDepthExEnvBool, DefaultsOnUnknown) {
    EXPECT_TRUE(parse_env_bool(std::nullopt, true));
    EXPECT_FALSE(parse_env_bool(std::nullopt, false));
    EXPECT_TRUE(parse_env_bool(std::optional<std::string_view>{"maybe"}, true));
}

TEST(TerminalDepthExMerge, MergesNew) {
    std::unordered_map<std::string, std::string> base{{"A", "1"}};
    std::unordered_map<std::string, std::string> over{{"B", "2"}};
    auto out = merge_env_overrides(base, over);
    EXPECT_EQ(out["A"], "1");
    EXPECT_EQ(out["B"], "2");
}

TEST(TerminalDepthExMerge, OverwritesExisting) {
    std::unordered_map<std::string, std::string> base{{"A", "1"}};
    std::unordered_map<std::string, std::string> over{{"A", "2"}};
    EXPECT_EQ(merge_env_overrides(base, over)["A"], "2");
}

TEST(TerminalDepthExMerge, EmptyValueUnsets) {
    std::unordered_map<std::string, std::string> base{{"A", "1"}, {"B", "2"}};
    std::unordered_map<std::string, std::string> over{{"A", ""}};
    auto out = merge_env_overrides(base, over);
    EXPECT_EQ(out.count("A"), 0u);
    EXPECT_EQ(out["B"], "2");
}

TEST(TerminalDepthExCleanup, EnvExpired) {
    EXPECT_TRUE(env_is_expired(300.0, 300.0));
    EXPECT_TRUE(env_is_expired(400.0, 300.0));
    EXPECT_FALSE(env_is_expired(299.9, 300.0));
}

TEST(TerminalDepthExCleanup, SelectExpired) {
    std::vector<std::pair<std::string, double>> envs{
        {"a", 100.0},
        {"b", 301.0},
        {"c", 50.0},
        {"d", 600.0},
    };
    auto ids = select_expired_env_ids(envs, 300.0);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "b");
    EXPECT_EQ(ids[1], "d");
}

TEST(TerminalDepthExDisk, BelowThresholdEmpty) {
    EXPECT_EQ(format_disk_usage_warning(50.0), "");
    EXPECT_EQ(format_disk_usage_warning(89.9), "");
}

TEST(TerminalDepthExDisk, AboveThresholdFormats) {
    auto msg = format_disk_usage_warning(95.0, 90.0);
    EXPECT_NE(msg.find("95"), std::string::npos);
    EXPECT_NE(msg.find("90"), std::string::npos);
}
