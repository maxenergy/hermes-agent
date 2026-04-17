// Tests for the `hermes acp` stdio dispatcher.
//
// We drive `hermes::cli::run_acp_stdio_loop` with std::stringstream
// substitutes for stdin / stdout, so the tests run fully in-process
// and deterministically without any real LLM provider.

#include "hermes/cli/acp_cmd.hpp"

#include "hermes/acp/acp_adapter.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace hermes::cli {
namespace {

// Hermetic adapter config: forced token so `initialize` works without
// any real ANTHROPIC/OPENAI env vars bleeding into the test process.
hermes::acp::AcpConfig make_cfg(const std::string& tok = "sk-test-bridge") {
    hermes::acp::AcpConfig c;
    c.forced_env_token = tok;
    return c;
}

// Parse `lines` written by the dispatcher into individual JSON objects.
std::vector<nlohmann::json> split_responses(const std::string& blob) {
    std::vector<nlohmann::json> out;
    std::stringstream ss(blob);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        out.push_back(nlohmann::json::parse(line));
    }
    return out;
}

TEST(AcpStdioLoop, InitializeReturnsServerCapabilities) {
    hermes::acp::AcpAdapter adapter(make_cfg());
    adapter.start();

    std::stringstream in;
    in << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
       << "\n";
    std::stringstream out;

    ASSERT_EQ(run_acp_stdio_loop(adapter, in, out), 0);
    auto responses = split_responses(out.str());
    ASSERT_EQ(responses.size(), 1u);

    const auto& env = responses[0];
    EXPECT_EQ(env.value("jsonrpc", ""), "2.0");
    EXPECT_EQ(env.value("id", -1), 1);
    ASSERT_TRUE(env.contains("result"));
    const auto& r = env["result"];
    // capabilities() returns name + protocol + auth_methods + capabilities.
    EXPECT_EQ(r.value("name", ""), "hermes");
    EXPECT_EQ(r.value("protocol", ""), "acp");
    ASSERT_TRUE(r.contains("auth_methods"));
    EXPECT_TRUE(r["auth_methods"].is_array());
    EXPECT_GE(r["auth_methods"].size(), 2u);
}

TEST(AcpStdioLoop, SessionNewReturnsNonEmptyId) {
    hermes::acp::AcpAdapter adapter(make_cfg());
    adapter.start();

    std::stringstream in;
    // The env-forced token grants implicit auth, so `session/new` works
    // without a prior `authenticate` round-trip.
    in << R"({"jsonrpc":"2.0","id":7,"method":"session/new","params":{}})"
       << "\n";
    std::stringstream out;

    ASSERT_EQ(run_acp_stdio_loop(adapter, in, out), 0);
    auto responses = split_responses(out.str());
    ASSERT_EQ(responses.size(), 1u);

    const auto& r = responses[0]["result"];
    EXPECT_EQ(r.value("status", ""), "ok");
    EXPECT_FALSE(r.value("session_id", std::string{}).empty());
}

TEST(AcpStdioLoop, PromptWithHandlerDoesNotReturnMethodNotAvailable) {
    hermes::acp::AcpAdapter adapter(make_cfg());
    adapter.start();

    // Install a deterministic handler — the point of the assertion is
    // that `prompt` is dispatched to the handler, not short-circuited
    // to `method_not_available`.
    adapter.set_prompt_handler(
        [](const nlohmann::json& params) -> nlohmann::json {
            return nlohmann::json{
                {"role", "assistant"},
                {"content",
                 std::string("echo: ") +
                     params.value("content", std::string{})}};
        });

    std::stringstream in;
    in << R"({"jsonrpc":"2.0","id":42,"method":"session/prompt",)"
       << R"("params":{"content":"hello"}})" << "\n";
    std::stringstream out;

    ASSERT_EQ(run_acp_stdio_loop(adapter, in, out), 0);
    auto responses = split_responses(out.str());
    ASSERT_EQ(responses.size(), 1u);

    const auto& r = responses[0]["result"];
    EXPECT_EQ(r.value("status", ""), "ok");
    ASSERT_TRUE(r.contains("result"));
    EXPECT_NE(r["result"].value("content", std::string{}).find("echo: hello"),
              std::string::npos);

    // The whole response must not mention the old stub-shaped error.
    EXPECT_EQ(responses[0].dump().find("method_not_available"),
              std::string::npos);
}

TEST(AcpStdioLoop, ParseErrorsSurfaceAsJsonRpcError) {
    hermes::acp::AcpAdapter adapter(make_cfg());
    adapter.start();

    std::stringstream in;
    in << "this is not json\n";
    std::stringstream out;

    ASSERT_EQ(run_acp_stdio_loop(adapter, in, out), 0);
    auto responses = split_responses(out.str());
    ASSERT_EQ(responses.size(), 1u);

    ASSERT_TRUE(responses[0].contains("error"));
    EXPECT_EQ(responses[0]["error"].value("code", 0), -32700);
}

TEST(AcpStdioLoop, ShutdownMethodBreaksLoop) {
    hermes::acp::AcpAdapter adapter(make_cfg());
    adapter.start();

    std::stringstream in;
    in << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
       << "\n";
    in << R"({"jsonrpc":"2.0","id":2,"method":"shutdown","params":{}})"
       << "\n";
    // This third line must NOT be processed because `shutdown` stops the loop.
    in << R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{}})"
       << "\n";
    std::stringstream out;

    ASSERT_EQ(run_acp_stdio_loop(adapter, in, out), 0);
    auto responses = split_responses(out.str());
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("id", -1), 1);
    EXPECT_EQ(responses[1].value("id", -1), 2);
}

}  // namespace
}  // namespace hermes::cli
