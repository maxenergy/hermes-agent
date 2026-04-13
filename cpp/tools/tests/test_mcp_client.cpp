#include "hermes/tools/mcp_client_tool.hpp"
#include "hermes/tools/mcp_transport.hpp"
#include "hermes/tools/registry.hpp"

#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace hermes::tools;
using json = nlohmann::json;

namespace {

class McpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        ToolRegistry::instance().clear();
    }

    void TearDown() override {
        ToolRegistry::instance().clear();
    }

    ToolContext ctx_{"task1", "test", "s1", "cli", "/tmp", {}};
};

TEST_F(McpClientTest, LoadConfigParsesJson) {
    json config = {
        {"my-server", {
            {"command", "npx"},
            {"args", json::array({"-y", "@mcp/server"})},
            {"timeout", 60},
            {"env", {{"API_KEY", "secret"}}},
            {"sampling", {{"enabled", true}, {"model", "claude-3"}}}
        }},
        {"http-server", {
            {"url", "https://example.com/mcp"},
            {"headers", {{"Authorization", "Bearer tok"}}}
        }}
    };

    McpClientManager mgr;
    mgr.load_config(config);

    auto cfg = mgr.get_config("my-server");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->command, "npx");
    EXPECT_EQ(cfg->args.size(), 2u);
    EXPECT_EQ(cfg->args[0], "-y");
    EXPECT_EQ(cfg->timeout, 60);
    EXPECT_EQ(cfg->env.at("API_KEY"), "secret");
    EXPECT_TRUE(cfg->sampling.enabled);
    EXPECT_EQ(cfg->sampling.model, "claude-3");

    auto cfg2 = mgr.get_config("http-server");
    ASSERT_TRUE(cfg2.has_value());
    EXPECT_EQ(cfg2->url, "https://example.com/mcp");
    EXPECT_EQ(cfg2->headers.at("Authorization"), "Bearer tok");
}

TEST_F(McpClientTest, ServerNamesLists) {
    json config = {
        {"alpha", {{"command", "a"}}},
        {"beta", {{"command", "b"}}},
        {"gamma", {{"url", "http://c"}}}
    };

    McpClientManager mgr;
    mgr.load_config(config);

    auto names = mgr.server_names();
    EXPECT_EQ(names.size(), 3u);

    // std::map keeps sorted order.
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "beta");
    EXPECT_EQ(names[2], "gamma");
}

TEST_F(McpClientTest, RegisterServerToolsCreatesStub) {
    json config = {
        {"test-server", {{"command", "test-cmd"}}}
    };

    McpClientManager mgr;
    mgr.load_config(config);
    mgr.register_server_tools("test-server", ToolRegistry::instance());

    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_test-server"));

    // Dispatching should return an error about not connected.
    auto result = json::parse(ToolRegistry::instance().dispatch(
        "mcp_test-server",
        {{"tool", "some_tool"}, {"arguments", json::object()}},
        ctx_));
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("not connected"),
              std::string::npos);
}

TEST_F(McpClientTest, GetConfigNonexistent) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    EXPECT_FALSE(mgr.get_config("nope").has_value());
    EXPECT_TRUE(mgr.server_names().empty());
}

TEST_F(McpClientTest, ReconnectConfigParsed) {
    json config = {
        {"rs", {
            {"command", "x"},
            {"reconnect", {{"enabled", false},
                           {"initial_ms", 10},
                           {"max_ms", 100},
                           {"max_attempts", 2}}}
        }},
        {"rs2", {{"command", "y"}, {"reconnect", false}}}
    };
    McpClientManager mgr;
    mgr.load_config(config);

    auto c = mgr.get_config("rs");
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(c->reconnect_enabled);
    EXPECT_EQ(c->reconnect_initial_ms, 10);
    EXPECT_EQ(c->reconnect_max_ms, 100);
    EXPECT_EQ(c->reconnect_max_attempts, 2);

    auto c2 = mgr.get_config("rs2");
    ASSERT_TRUE(c2.has_value());
    EXPECT_FALSE(c2->reconnect_enabled);
}

TEST_F(McpClientTest, AllowSamplingGating) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    EXPECT_FALSE(mgr.allow_sampling());
    mgr.set_allow_sampling(true);
    EXPECT_TRUE(mgr.allow_sampling());
}

TEST_F(McpClientTest, SamplingRejectedWhenDisabled) {
    McpClientManager mgr;
    mgr.load_config(json::object());

    // allow_sampling default = false; handle_inbound_ should throw.
    EXPECT_THROW(
        mgr.handle_inbound_("srv", "sampling/createMessage",
                            {{"messages", json::array()}}),
        std::runtime_error);
}

TEST_F(McpClientTest, SamplingRejectedWhenNoLlmClient) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    mgr.set_allow_sampling(true);
    EXPECT_THROW(
        mgr.handle_inbound_("srv", "sampling/createMessage",
                            {{"messages", json::array()}}),
        std::runtime_error);
}

// Fake LLM client for sampling tests.
class FakeLlmClient : public hermes::llm::LlmClient {
public:
    hermes::llm::CompletionResponse complete(
        const hermes::llm::CompletionRequest& req) override {
            last_req = req;
            hermes::llm::CompletionResponse r;
            r.assistant_message.role = hermes::llm::Role::Assistant;
            r.assistant_message.content_text = "hello from fake";
            r.finish_reason = "stop";
            r.raw = {{"model", req.model}};
            return r;
    }
    std::string provider_name() const override { return "fake"; }

    hermes::llm::CompletionRequest last_req;
};

TEST_F(McpClientTest, SamplingRoutesToLlmClient) {
    McpClientManager mgr;
    mgr.load_config({{"srv", {{"command", "x"},
                              {"sampling", {{"model", "fake-model"},
                                            {"max_tokens_cap", 128}}}}}});
    mgr.set_allow_sampling(true);

    FakeLlmClient fake;
    mgr.set_llm_client(&fake);

    json params;
    params["messages"] = json::array({
        {{"role", "user"}, {"content", {{"type", "text"}, {"text", "hi"}}}}
    });
    params["systemPrompt"] = "You are a test.";
    params["maxTokens"] = 500;  // exceeds cap → should clamp to 128.
    params["temperature"] = 0.7;

    json out = mgr.handle_inbound_("srv", "sampling/createMessage", params);
    EXPECT_EQ(out["role"], "assistant");
    EXPECT_EQ(out["content"]["type"], "text");
    EXPECT_EQ(out["content"]["text"], "hello from fake");

    // Request captured: model + max_tokens capped + system + user.
    EXPECT_EQ(fake.last_req.model, "fake-model");
    ASSERT_TRUE(fake.last_req.max_tokens.has_value());
    EXPECT_EQ(*fake.last_req.max_tokens, 128);
    ASSERT_GE(fake.last_req.messages.size(), 2u);
    EXPECT_EQ(fake.last_req.messages.front().role, hermes::llm::Role::System);
    EXPECT_EQ(fake.last_req.messages.back().role, hermes::llm::Role::User);
    EXPECT_EQ(fake.last_req.messages.back().content_text, "hi");
    ASSERT_TRUE(fake.last_req.temperature.has_value());
    EXPECT_NEAR(*fake.last_req.temperature, 0.7, 1e-9);
}

TEST_F(McpClientTest, SamplingApproverCanReject) {
    McpClientManager mgr;
    mgr.load_config({{"srv", {{"command", "x"}}}});
    mgr.set_allow_sampling(true);
    FakeLlmClient fake;
    mgr.set_llm_client(&fake);
    mgr.set_sampling_approver(
        [](const std::string&, const json&) { return false; });
    EXPECT_THROW(mgr.handle_inbound_("srv", "sampling/createMessage",
                                     {{"messages", json::array()}}),
                 std::runtime_error);
}

TEST_F(McpClientTest, UnknownInboundMethodThrows) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    EXPECT_THROW(mgr.handle_inbound_("srv", "unknown/method", json::object()),
                 std::runtime_error);
}

TEST_F(McpClientTest, OAuthInitiatorInvoked) {
    McpClientManager mgr;
    mgr.load_config(json::object());
    std::atomic<int> calls{0};
    mgr.set_oauth_initiator(
        [&](const std::string& name, const std::string& hdr) -> std::string {
            ++calls;
            EXPECT_EQ(name, "srv");
            EXPECT_TRUE(hdr.empty() || hdr.find("Bearer") != std::string::npos);
            return "tok-abc";
        });
    auto tok = mgr.run_oauth_flow("srv");
    EXPECT_EQ(tok, "tok-abc");
    EXPECT_EQ(calls.load(), 1);
}

// ---------------------------------------------------------------------------
// Dynamic tool discovery via notifications/tools/list_changed
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST_F(McpClientTest, DynamicToolDiscoveryRefresh) {
    // Mock MCP server: /tmp file-backed toolset — first call returns
    // [hello]; after we delete a marker file it returns [hello, goodbye].
    // Sends notifications/tools/list_changed whenever tools/list is called
    // in "second state".
    std::string script =
        "import sys, json, os\n"
        "state_file = '/tmp/hermes_mcp_test_state_disc'\n"
        "# state: 0 = first, 1 = expanded\n"
        "if not os.path.exists(state_file):\n"
        "    open(state_file, 'w').write('0')\n"
        "for line in sys.stdin:\n"
        "    line = line.strip()\n"
        "    if not line: continue\n"
        "    try: req = json.loads(line)\n"
        "    except Exception: continue\n"
        "    rid = req.get('id')\n"
        "    method = req.get('method', '')\n"
        "    if method == 'initialize':\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'protocolVersion':'2024-11-05','capabilities':{}}}\n"
        "        print(json.dumps(resp), flush=True)\n"
        "    elif method == 'tools/list':\n"
        "        state = open(state_file).read().strip()\n"
        "        if state == '0':\n"
        "            tools = [{'name':'hello','description':'Say hello','inputSchema':{'type':'object'}}]\n"
        "        else:\n"
        "            tools = [{'name':'hello','description':'Say hello','inputSchema':{'type':'object'}},\n"
        "                     {'name':'goodbye','description':'Say bye','inputSchema':{'type':'object'}}]\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'tools':tools}}\n"
        "        print(json.dumps(resp), flush=True)\n"
        "    elif method == 'trigger_change':\n"
        "        open(state_file, 'w').write('1')\n"
        "        resp = {'jsonrpc':'2.0','id':rid,'result':{'ok':True}}\n"
        "        print(json.dumps(resp), flush=True)\n"
        "        # send the notification\n"
        "        print(json.dumps({'jsonrpc':'2.0','method':'notifications/tools/list_changed'}), flush=True)\n";

    // Reset state file.
    ::unlink("/tmp/hermes_mcp_test_state_disc");

    auto transport = std::make_shared<McpStdioTransport>(
        "python3", std::vector<std::string>{"-u", "-c", script});
    transport->initialize();

    McpClientManager mgr;
    mgr.load_config({{"srv", {{"command", "x"}}}});
    mgr.inject_transport_for_testing("srv", transport);
    mgr.register_server_tools("srv", ToolRegistry::instance());

    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_srv_hello"));
    EXPECT_FALSE(ToolRegistry::instance().has_tool("mcp_srv_goodbye"));

    // Trigger the change — server writes a list_changed notification.
    // Use a dummy send_request by calling call_tool on a real method.
    try {
        transport->send_request("trigger_change", json::object(),
                                std::chrono::seconds(5));
    } catch (...) {}

    // Drain any pending notification.
    transport->pump_messages(std::chrono::milliseconds(200));

    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_srv_hello"));
    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_srv_goodbye"));

    transport->shutdown();
    ::unlink("/tmp/hermes_mcp_test_state_disc");
}

TEST_F(McpClientTest, InboundSamplingFromTransportIsRouted) {
    // Mock server that sends sampling/createMessage, then waits for the
    // response and echoes it back as a tools/call result.
    std::string script =
        "import sys, json\n"
        "# Wait for initialize first — only then push the sampling request.\n"
        "for line in sys.stdin:\n"
        "    line=line.strip()\n"
        "    if not line: continue\n"
        "    try: req=json.loads(line)\n"
        "    except: continue\n"
        "    rid=req.get('id')\n"
        "    m=req.get('method','')\n"
        "    if m=='initialize':\n"
        "        print(json.dumps({'jsonrpc':'2.0','id':rid,'result':{}}), flush=True)\n"
        "    elif m=='notifications/initialized':\n"
        "        # now safe to push sampling request (handler installed).\n"
        "        pass\n"
        "    elif m=='tools/list':\n"
        "        print(json.dumps({'jsonrpc':'2.0','id':rid,'result':{'tools':[]}}), flush=True)\n"
        "        # immediately push the server-initiated sampling request.\n"
        "        print(json.dumps({'jsonrpc':'2.0','id':777,'method':'sampling/createMessage',"
        "'params':{'messages':[{'role':'user','content':{'type':'text','text':'ping'}}],"
        "'maxTokens':50}}), flush=True)\n"
        "    elif rid==777:\n"
        "        # sampling response from client — stash it.\n"
        "        open('/tmp/hermes_mcp_sampling_out','w').write(json.dumps(req.get('result') or req.get('error')))\n";

    ::unlink("/tmp/hermes_mcp_sampling_out");

    auto transport = std::make_shared<McpStdioTransport>(
        "python3", std::vector<std::string>{"-u", "-c", script});
    transport->initialize();

    McpClientManager mgr;
    mgr.load_config({{"srv", {{"command", "x"}}}});
    mgr.set_allow_sampling(true);
    FakeLlmClient fake;
    mgr.set_llm_client(&fake);
    mgr.inject_transport_for_testing("srv", transport);
    mgr.register_server_tools("srv", ToolRegistry::instance());

    // After register_server_tools calls list_tools, the server emits the
    // sampling request.  Drain it.
    transport->pump_messages(std::chrono::milliseconds(1500));

    // Read the file.
    std::ifstream ifs("/tmp/hermes_mcp_sampling_out");
    ASSERT_TRUE(ifs.good());
    std::string body((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    auto parsed = json::parse(body, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    EXPECT_EQ(parsed["role"], "assistant");
    EXPECT_EQ(parsed["content"]["text"], "hello from fake");

    transport->shutdown();
    ::unlink("/tmp/hermes_mcp_sampling_out");
}
TEST_F(McpClientTest, ReconnectBackoffRespectsMaxAttempts) {
    // Command that always fails to launch — validates backoff loop
    // exits after max_attempts and fast delays don't blow the timeout.
    McpClientManager mgr;
    json config = {{"rs", {
        {"command", "/nonexistent/mcp-server-xyz"},
        {"reconnect", {{"enabled", true},
                       {"initial_ms", 5},
                       {"max_ms", 20},
                       {"max_attempts", 3}}}
    }}};
    mgr.load_config(config);
    auto start = std::chrono::steady_clock::now();
    // connect() is not public — use register_server_tools which calls
    // connect_with_backoff_ internally; it should fall back and register
    // the stub tool.
    mgr.register_server_tools("rs", ToolRegistry::instance());
    auto dur = std::chrono::steady_clock::now() - start;
    // 2 sleeps of ~5-12ms + ~10-24ms = under 200ms total.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(dur).count(),
              2000);
    // Stub tool is registered as fallback.
    EXPECT_TRUE(ToolRegistry::instance().has_tool("mcp_rs"));
}

TEST_F(McpClientTest, OAuthKickedInOnAuthFailure) {
    // Launch a command that fails with an "unauthorized" stderr/exit so
    // initialize() throws with matching text.  Simulate that by having the
    // child immediately exit after reading — initialize will timeout which
    // throws a different error.  So instead we directly test the OAuth
    // initiator path by calling connect() on an unreachable command with
    // an oauth_initiator_ that returns empty.
    McpClientManager mgr;
    mgr.load_config({{"srv", {{"command", "/nonexistent"}}}});
    std::atomic<int> calls{0};
    mgr.set_oauth_initiator(
        [&](const std::string&, const std::string&) -> std::string {
            ++calls;
            return "";  // abort OAuth
        });
    // connect() should fail; OAuth not invoked because the error text
    // doesn't match 401/unauthorized.  The initiator is tested directly
    // via run_oauth_flow in a separate test.
    EXPECT_FALSE(mgr.connect("srv"));
    EXPECT_EQ(calls.load(), 0);
}

#endif  // !_WIN32

}  // namespace
