#include "hermes/tools/mcp_transport.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace hermes::tools;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// JsonRpcMessage serialization tests
// ---------------------------------------------------------------------------

TEST(JsonRpcMessageTest, RequestRoundTrip) {
    JsonRpcMessage msg;
    msg.id = "42";
    msg.method = "tools/list";
    msg.params = json::object();

    auto j = msg.to_json();
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 42);
    EXPECT_EQ(j["method"], "tools/list");

    auto parsed = JsonRpcMessage::from_json(j);
    EXPECT_EQ(parsed.id.value(), "42");
    EXPECT_EQ(parsed.method, "tools/list");
    EXPECT_TRUE(parsed.is_request());
    EXPECT_FALSE(parsed.is_response());
}

TEST(JsonRpcMessageTest, ResponseRoundTrip) {
    JsonRpcMessage msg;
    msg.id = "1";
    msg.result = json{{"tools", json::array()}};

    auto j = msg.to_json();
    EXPECT_EQ(j["id"], 1);
    EXPECT_TRUE(j.contains("result"));
    EXPECT_FALSE(j.contains("method"));

    auto parsed = JsonRpcMessage::from_json(j);
    EXPECT_TRUE(parsed.is_response());
    EXPECT_FALSE(parsed.is_request());
    EXPECT_EQ(parsed.result["tools"].size(), 0u);
}

TEST(JsonRpcMessageTest, ErrorResponse) {
    json j = {
        {"jsonrpc", "2.0"},
        {"id", 5},
        {"error", {{"code", -32600}, {"message", "Invalid Request"}}}
    };

    auto msg = JsonRpcMessage::from_json(j);
    EXPECT_TRUE(msg.is_response());
    EXPECT_FALSE(msg.is_request());
    EXPECT_EQ(msg.error["code"], -32600);
}

TEST(JsonRpcMessageTest, NotificationHasNoId) {
    JsonRpcMessage msg;
    msg.method = "notifications/initialized";

    auto j = msg.to_json();
    EXPECT_FALSE(j.contains("id"));
    EXPECT_EQ(j["method"], "notifications/initialized");
}

// ---------------------------------------------------------------------------
// build_child_env tests
// ---------------------------------------------------------------------------

TEST(McpTransportEnvTest, SafeVarsPassedThrough) {
    auto env = McpStdioTransport::build_child_env({});
    // PATH should be present (it's almost always set).
    if (std::getenv("PATH")) {
        EXPECT_TRUE(env.count("PATH") > 0);
    }
}

TEST(McpTransportEnvTest, ServerEnvMerged) {
    auto env = McpStdioTransport::build_child_env({{"MY_VAR", "hello"}});
    EXPECT_EQ(env["MY_VAR"], "hello");
}

TEST(McpTransportEnvTest, SensitiveVarsFiltered) {
    // Server env contains a sensitive var that should be filtered.
    auto env = McpStdioTransport::build_child_env({
        {"OPENAI_API_KEY", "sk-secret"},
        {"SAFE_VAR", "ok"}
    });
    EXPECT_EQ(env.count("OPENAI_API_KEY"), 0u);
    EXPECT_EQ(env["SAFE_VAR"], "ok");
}

TEST(McpTransportEnvTest, SuffixSensitiveFiltered) {
    auto env = McpStdioTransport::build_child_env({
        {"MY_SERVICE_API_KEY", "secret"},
        {"MY_SERVICE_HOST", "localhost"}
    });
    EXPECT_EQ(env.count("MY_SERVICE_API_KEY"), 0u);
    EXPECT_EQ(env["MY_SERVICE_HOST"], "localhost");
}

// ---------------------------------------------------------------------------
// write_message / read_message via pipe (self-pipe test)
// ---------------------------------------------------------------------------

#ifndef _WIN32

#include <unistd.h>

TEST(McpTransportTest, WriteAndReadMessageViaPipe) {
    // Use a bash echo-back script as a mock MCP server.
    // The script reads a line from stdin, then writes it back to stdout.
    std::vector<std::string> args = {
        "-c",
        "while IFS= read -r line; do echo \"$line\"; done"
    };
    McpStdioTransport transport("bash", args);

    EXPECT_TRUE(transport.is_connected());

    // Send a message and read it back.
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "test"},
        {"params", json::object()}
    };

    transport.write_message(msg);
    auto result = transport.read_message(std::chrono::seconds(5));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)["jsonrpc"], "2.0");
    EXPECT_EQ((*result)["id"], 1);
    EXPECT_EQ((*result)["method"], "test");

    transport.shutdown();
    EXPECT_FALSE(transport.is_connected());
}

TEST(McpTransportTest, SendRequestAndReceiveResponse) {
    // Mock MCP server: reads JSON-RPC request, returns a response with
    // matching id.  Uses python3 to parse the id from each line.
    std::string script =
        "while IFS= read -r line; do "
        "  id=$(echo \"$line\" | python3 -c "
        "\"import sys,json; print(json.load(sys.stdin).get('id',''))\" "
        "2>/dev/null); "
        "  if [ -n \"$id\" ]; then "
        "    echo \"{\\\"jsonrpc\\\":\\\"2.0\\\",\\\"id\\\":$id,"
        "\\\"result\\\":{\\\"ok\\\":true}}\"; "
        "  fi; "
        "done";

    std::vector<std::string> args = {"-c", script};
    McpStdioTransport transport("bash", args);
    EXPECT_TRUE(transport.is_connected());

    auto result = transport.send_request("test/method", json::object(),
                                         std::chrono::seconds(10));
    EXPECT_TRUE(result.contains("ok"));
    EXPECT_EQ(result["ok"], true);

    transport.shutdown();
}

TEST(McpTransportTest, TimeoutOnUnresponsiveServer) {
    // Server that never responds.
    std::vector<std::string> args = {"-c", "sleep 60"};
    McpStdioTransport transport("bash", args);
    EXPECT_TRUE(transport.is_connected());

    EXPECT_THROW(
        transport.send_request("test", json::object(), std::chrono::seconds(1)),
        std::runtime_error);

    transport.shutdown();
}

TEST(McpTransportTest, InitializeHandshake) {
    // Mock MCP server implemented as a Python script for reliable JSON parsing.
    std::string script =
        "import sys, json\n"
        "for line in sys.stdin:\n"
        "    line = line.strip()\n"
        "    if not line:\n"
        "        continue\n"
        "    try:\n"
        "        req = json.loads(line)\n"
        "    except Exception:\n"
        "        continue\n"
        "    rid = req.get('id')\n"
        "    method = req.get('method', '')\n"
        "    if method == 'initialize':\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'protocolVersion':'2024-11-05','capabilities':{},'serverInfo':{'name':'test','version':'0.1'}}}\n"
        "    elif method == 'tools/list':\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'tools':[{'name':'hello','description':'Say hello','inputSchema':{'type':'object','properties':{'name':{'type':'string'}}}}]}}\n"
        "    elif method == 'tools/call':\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'content':[{'type':'text','text':'Hello, world!'}]}}\n"
        "    else:\n"
        "        continue\n"
        "    print(json.dumps(resp), flush=True)\n";

    std::vector<std::string> args = {"-u", "-c", script};
    McpStdioTransport transport("python3", args);

    auto init_result = transport.initialize("test-client", "0.0.1");
    EXPECT_EQ(init_result["protocolVersion"], "2024-11-05");

    auto tools = transport.list_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]["name"], "hello");

    auto call_result = transport.call_tool("hello", {{"name", "world"}});
    EXPECT_TRUE(call_result.contains("content"));

    transport.shutdown();
}

// Gate live MCP server tests with MCP_TEST=1.
class McpLiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* mcp_test = std::getenv("MCP_TEST");
        if (!mcp_test || std::string(mcp_test) != "1") {
            GTEST_SKIP() << "MCP_TEST=1 not set, skipping live tests";
        }
    }
};

TEST_F(McpLiveTest, LiveServerConnection) {
    // Placeholder for live MCP server tests.
    GTEST_SKIP() << "Placeholder for live MCP server tests";
}

#endif  // _WIN32

}  // namespace
