// Tests for hermes/tools/code_execution_depth.hpp.
#include "hermes/tools/code_execution_depth.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace hermes::tools::code_execution::depth;

namespace {

bool always_false(const std::string&) {
    return false;
}

bool allow_special(const std::string& k) {
    return k == "MY_SKILL_VAR";
}

}  // namespace

TEST(CodeExecDepthEnv, SecretSubstrBlocksByDefault) {
    EXPECT_TRUE(env_name_looks_like_secret("OPENAI_API_KEY"));
    EXPECT_TRUE(env_name_looks_like_secret("GITHUB_TOKEN"));
    EXPECT_TRUE(env_name_looks_like_secret("aws_secret_access_key"));
    EXPECT_TRUE(env_name_looks_like_secret("MY_AUTH_CREDENTIAL"));
    EXPECT_FALSE(env_name_looks_like_secret("PATH"));
    EXPECT_FALSE(env_name_looks_like_secret("HOME"));
}

TEST(CodeExecDepthEnv, SafePrefixAllowed) {
    EXPECT_TRUE(env_name_has_safe_prefix("PATH"));
    EXPECT_TRUE(env_name_has_safe_prefix("XDG_CONFIG_HOME"));
    EXPECT_TRUE(env_name_has_safe_prefix("LC_ALL"));
    EXPECT_TRUE(env_name_has_safe_prefix("CONDA_PREFIX"));
    EXPECT_FALSE(env_name_has_safe_prefix("RANDOM_VAR"));
}

TEST(CodeExecDepthEnv, FilterChildEnvBlocksSecrets) {
    std::unordered_map<std::string, std::string> src = {
        {"PATH", "/usr/bin"},
        {"HOME", "/home/u"},
        {"OPENAI_API_KEY", "sk-..."},
        {"RANDOM", "42"},
        {"XDG_CACHE_HOME", "/home/u/.cache"},
    };
    auto out = filter_child_env(src, always_false);
    EXPECT_EQ(out.count("PATH"), 1u);
    EXPECT_EQ(out.count("HOME"), 1u);
    EXPECT_EQ(out.count("OPENAI_API_KEY"), 0u);
    EXPECT_EQ(out.count("RANDOM"), 0u);
    EXPECT_EQ(out.count("XDG_CACHE_HOME"), 1u);
}

TEST(CodeExecDepthEnv, PassthroughOverridesSecretBlock) {
    std::unordered_map<std::string, std::string> src = {
        {"MY_SKILL_VAR", "v"},
        {"OPENAI_API_KEY", "sk"},
    };
    auto out = filter_child_env(src, allow_special);
    EXPECT_EQ(out.count("MY_SKILL_VAR"), 1u);
    EXPECT_EQ(out.count("OPENAI_API_KEY"), 0u);
}

TEST(CodeExecDepthEnv, NullPassthroughTreatedAsNone) {
    std::unordered_map<std::string, std::string> src = {
        {"PATH", "/usr/bin"}, {"MY_SKILL_VAR", "v"}};
    auto out = filter_child_env(src, nullptr);
    EXPECT_EQ(out.count("PATH"), 1u);
    EXPECT_EQ(out.count("MY_SKILL_VAR"), 0u);
}

TEST(CodeExecDepthForbiddenParams, Covered) {
    const auto& s = forbidden_terminal_params();
    EXPECT_EQ(s.count("background"), 1u);
    EXPECT_EQ(s.count("pty"), 1u);
    EXPECT_EQ(s.count("force"), 1u);
}

TEST(CodeExecDepthForbiddenParams, StripDrops) {
    nlohmann::json args;
    args["command"] = "ls";
    args["background"] = true;
    args["pty"] = true;
    args["force"] = true;
    EXPECT_EQ(strip_forbidden_terminal_params(args), 3u);
    EXPECT_EQ(args.count("command"), 1u);
    EXPECT_EQ(args.count("background"), 0u);
}

TEST(CodeExecDepthForbiddenParams, StripNonObject) {
    nlohmann::json args = nlohmann::json::array({1, 2});
    EXPECT_EQ(strip_forbidden_terminal_params(args), 0u);
}

TEST(CodeExecDepthRpc, ParseFramesBasic) {
    std::string buf = R"({"tool":"a","args":{}}
{"tool":"b","args":{"x":1}}
)";
    auto out = parse_rpc_frames(buf);
    EXPECT_EQ(out.frames.size(), 2u);
    EXPECT_EQ(out.residual, "");
    EXPECT_EQ(out.frames.at(1).at("tool").get<std::string>(), "b");
}

TEST(CodeExecDepthRpc, ParseFramesPartialTail) {
    std::string buf = R"({"tool":"a"}
{"tool":"b")";
    auto out = parse_rpc_frames(buf);
    EXPECT_EQ(out.frames.size(), 1u);
    EXPECT_EQ(out.residual, R"({"tool":"b")");
}

TEST(CodeExecDepthRpc, ParseFramesInvalidJson) {
    std::string buf = "not-json\n{\"ok\":true}\n";
    auto out = parse_rpc_frames(buf);
    EXPECT_EQ(out.frames.size(), 1u);
    EXPECT_EQ(out.parse_errors, 1u);
}

TEST(CodeExecDepthRpc, ParseFramesBlankLinesSkipped) {
    std::string buf = "\n\n{\"a\":1}\n\n";
    auto out = parse_rpc_frames(buf);
    EXPECT_EQ(out.frames.size(), 1u);
    EXPECT_EQ(out.parse_errors, 0u);
}

TEST(CodeExecDepthRpc, EncodeReply) {
    EXPECT_EQ(encode_rpc_reply("{\"a\":1}"), "{\"a\":1}\n");
}

TEST(CodeExecDepthRpc, NotAllowedReplySorts) {
    std::unordered_set<std::string> allowed{"zzz", "aaa", "mmm"};
    auto msg = build_not_allowed_reply("bad", allowed);
    auto parsed = nlohmann::json::parse(msg);
    const auto err = parsed.at("error").get<std::string>();
    EXPECT_NE(err.find("aaa, mmm, zzz"), std::string::npos);
    EXPECT_NE(err.find("'bad'"), std::string::npos);
}

TEST(CodeExecDepthRpc, LimitReply) {
    auto parsed = nlohmann::json::parse(build_limit_reached_reply(42));
    EXPECT_NE(parsed.at("error").get<std::string>().find("(42)"),
              std::string::npos);
}

TEST(CodeExecDepthRpc, InvalidRpcReply) {
    auto parsed = nlohmann::json::parse(build_invalid_rpc_reply("oops"));
    EXPECT_NE(parsed.at("error").get<std::string>().find("oops"),
              std::string::npos);
}

TEST(CodeExecDepthSocket, ResolveTmpdirMac) {
    EXPECT_EQ(resolve_socket_tmpdir("darwin", "/var/folders/xyz"), "/tmp");
}

TEST(CodeExecDepthSocket, ResolveTmpdirLinux) {
    EXPECT_EQ(resolve_socket_tmpdir("linux", "/tmp"), "/tmp");
    EXPECT_EQ(resolve_socket_tmpdir("linux", "/custom/tmp"), "/custom/tmp");
}

TEST(CodeExecDepthSocket, BuildPath) {
    EXPECT_EQ(build_socket_path("/tmp", "deadbeef"),
              "/tmp/hermes_rpc_deadbeef.sock");
    EXPECT_EQ(build_socket_path("/tmp/", "abc"), "/tmp/hermes_rpc_abc.sock");
}

TEST(CodeExecDepthPreview, ShortObject) {
    nlohmann::json args;
    args["x"] = 1;
    auto p = format_args_preview(args);
    EXPECT_EQ(p, "{'x': 1}");
}

TEST(CodeExecDepthPreview, TruncatedAt80) {
    nlohmann::json args;
    args["long"] = std::string(200, 'z');
    auto p = format_args_preview(args, 80);
    EXPECT_EQ(p.size(), 80u);
}

TEST(CodeExecDepthPreview, StringRepr) {
    nlohmann::json args = "hello";
    auto p = format_args_preview(args);
    EXPECT_EQ(p, "\"hello\"");
}

TEST(CodeExecDepthPreview, RoundDuration) {
    EXPECT_DOUBLE_EQ(round_duration_seconds(1.23456), 1.23);
    EXPECT_DOUBLE_EQ(round_duration_seconds(0.999), 1.0);
}

TEST(CodeExecDepthSchema, CanonicalDocHasSeven) {
    EXPECT_EQ(canonical_tool_doc_lines().size(), 7u);
}

TEST(CodeExecDepthSchema, FilterDocLinesOrdered) {
    auto out = filter_tool_doc_lines({"terminal", "web_search"});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out.at(0).first, "web_search");
    EXPECT_EQ(out.at(1).first, "terminal");
}

TEST(CodeExecDepthSchema, FilterDocLinesEmpty) {
    auto out = filter_tool_doc_lines({});
    EXPECT_TRUE(out.empty());
}

TEST(CodeExecDepthSchema, ImportExamplesPreferred) {
    EXPECT_EQ(build_import_examples({"web_search", "terminal", "write_file"}),
              "web_search, terminal, ...");
}

TEST(CodeExecDepthSchema, ImportExamplesFallback) {
    EXPECT_EQ(build_import_examples({"patch", "read_file"}),
              "patch, read_file, ...");
}

TEST(CodeExecDepthSchema, ImportExamplesEmpty) {
    EXPECT_EQ(build_import_examples({}), "...");
}

TEST(CodeExecDepthSchema, WindowsReply) {
    auto parsed = nlohmann::json::parse(windows_unsupported_reply());
    EXPECT_NE(parsed.at("error").get<std::string>().find("Windows"),
              std::string::npos);
}

TEST(CodeExecDepthClamp, ClampMaxToolCalls) {
    EXPECT_EQ(clamp_max_tool_calls(0, 30), 30);
    EXPECT_EQ(clamp_max_tool_calls(-1, 30), 30);
    EXPECT_EQ(clamp_max_tool_calls(9999, 30), kMaxToolCalls);
    EXPECT_EQ(clamp_max_tool_calls(10, 30), 10);
}
