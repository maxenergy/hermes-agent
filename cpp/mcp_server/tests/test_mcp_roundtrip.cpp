// HTTP/SSE round-trip tests for the native MCP server.
//
// These tests spin up ``McpServer`` on a random localhost port, then drive
// the three core JSON-RPC methods (initialize / tools/list / tools/call)
// through a small raw-socket client. We also exercise error paths
// (unknown method, parse error) and the session GC.
#include "hermes/mcp_server/mcp_server.hpp"
#include "hermes/mcp_server/rpc_dispatch.hpp"
#include "hermes/mcp_server/session.hpp"
#include "hermes/mcp_server/rpc_types.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::mcp_server {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// ---------- Minimal HTTP client ------------------------------------------

struct HttpReply {
    int status = 0;
    std::string body;
    std::string headers;
};

HttpReply http_post(std::uint16_t port, const std::string& target,
                    const std::string& body,
                    const std::string& extra_headers = "") {
    asio::io_context io;
    tcp::socket sock(io);
    tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
    sock.connect(ep);

    std::ostringstream req;
    req << "POST " << target << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << extra_headers
        << "\r\n"
        << body;
    auto s = req.str();
    asio::write(sock, asio::buffer(s));

    std::string raw;
    boost::system::error_code ec;
    char buf[4096];
    while (true) {
        std::size_t n = sock.read_some(asio::buffer(buf), ec);
        if (n > 0) raw.append(buf, n);
        if (ec) break;
    }

    HttpReply out;
    auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        out.body = raw;
        return out;
    }
    out.headers = raw.substr(0, sep);
    out.body = raw.substr(sep + 4);
    // Parse "HTTP/1.1 <code> ..."
    auto sp1 = out.headers.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = out.headers.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) {
            out.status = std::atoi(out.headers.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
        }
    }
    return out;
}

// ---------- Test fixture --------------------------------------------------

class McpRoundtripTest : public ::testing::Test {
protected:
    void SetUp() override {
        McpServer::Options opts;
        opts.bind_address = "127.0.0.1";
        opts.port = 0;
        opts.worker_threads = 4;
        opts.gc_interval = std::chrono::seconds(3600);
        server_ = std::make_unique<McpServer>(opts);

        // Install a canned tool call hook + resource / prompt providers
        // so we can assert the full method surface without wiring the
        // real ``hermes::tools::ToolRegistry``.
        server_->set_tool_call_hook(
            [this](const std::string& name, const nlohmann::json& args) {
                last_tool_name_ = name;
                last_tool_args_ = args;
                nlohmann::json out;
                out["echo"] = args;
                out["name"] = name;
                return out;
            });

        auto res = std::make_shared<ResourceProvider>();
        res->list = [] {
            nlohmann::json r = nlohmann::json::array();
            r.push_back({{"uri", "hermes://readme"}, {"name", "README"}});
            return r;
        };
        res->read = [](const std::string& uri) {
            nlohmann::json out;
            out["contents"] = nlohmann::json::array();
            out["contents"].push_back({{"uri", uri}, {"text", "hello"}});
            return out;
        };
        res->list_templates = [] {
            nlohmann::json r = nlohmann::json::array();
            r.push_back({{"uriTemplate", "hermes://sessions/{id}"},
                         {"name", "session"},
                         {"description", "session transcript"}});
            return r;
        };
        server_->register_resource_provider(res);

        auto prom = std::make_shared<PromptProvider>();
        prom->list = [] {
            nlohmann::json r = nlohmann::json::array();
            r.push_back({{"name", "greet"}, {"description", "say hi"}});
            return r;
        };
        prom->get = [](const std::string& name, const nlohmann::json& args) {
            nlohmann::json r;
            r["messages"] = nlohmann::json::array();
            r["messages"].push_back({{"role", "user"},
                                     {"content", {{"type", "text"},
                                                  {"text", name + " " + args.dump()}}}});
            return r;
        };
        server_->register_prompt_provider(prom);

        port_ = server_->start();
        ASSERT_NE(port_, 0u);
    }

    void TearDown() override {
        server_->stop();
        server_.reset();
    }

    std::string last_tool_name_;
    nlohmann::json last_tool_args_;
    std::unique_ptr<McpServer> server_;
    std::uint16_t port_ = 0;
};

// ---------- Handshake + tools --------------------------------------------

TEST_F(McpRoundtripTest, InitializeReturnsProtocolVersion) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = "initialize";
    req["params"] = {{"protocolVersion", "2024-11-05"},
                     {"clientInfo", {{"name", "test"}, {"version", "1"}}}};

    auto rep = http_post(port_, "/", req.dump());
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 1);
    ASSERT_TRUE(j.contains("result"));
    EXPECT_EQ(j["result"]["protocolVersion"], "2024-11-05");
    EXPECT_TRUE(j["result"]["capabilities"].contains("tools"));
    EXPECT_EQ(j["result"]["serverInfo"]["name"], "hermes-mcp");
}

TEST_F(McpRoundtripTest, PingReturnsEmptyObject) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = "p1";
    req["method"] = "ping";
    auto rep = http_post(port_, "/", req.dump());
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
    EXPECT_TRUE(j["result"].is_object());
    EXPECT_EQ(j["result"].size(), 0u);
}

TEST_F(McpRoundtripTest, ToolsListAlwaysReturnsToolsArray) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 2;
    req["method"] = "tools/list";
    auto rep = http_post(port_, "/", req.dump());
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j["result"].contains("tools"));
    EXPECT_TRUE(j["result"]["tools"].is_array());
    // No registry configured in this fixture → empty is acceptable.
    EXPECT_GE(j["result"]["tools"].size(), 0u);
}

TEST_F(McpRoundtripTest, ToolsCallInvokesHook) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 3;
    req["method"] = "tools/call";
    req["params"] = {{"name", "echo_tool"}, {"arguments", {{"hello", "world"}}}};
    auto rep = http_post(port_, "/", req.dump());
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
    ASSERT_TRUE(j["result"].contains("content"));
    ASSERT_TRUE(j["result"]["content"].is_array());
    EXPECT_GE(j["result"]["content"].size(), 1u);
    EXPECT_EQ(j["result"]["content"][0]["type"], "text");
    EXPECT_EQ(last_tool_name_, "echo_tool");
    EXPECT_EQ(last_tool_args_["hello"], "world");
}

TEST_F(McpRoundtripTest, ResourcesListAndRead) {
    auto list_req =
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 10}, {"method", "resources/list"}};
    auto rep = http_post(port_, "/", list_req.dump());
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j["result"]["resources"].is_array());
    ASSERT_EQ(j["result"]["resources"].size(), 1u);

    nlohmann::json read_req;
    read_req["jsonrpc"] = "2.0";
    read_req["id"] = 11;
    read_req["method"] = "resources/read";
    read_req["params"] = {{"uri", "hermes://readme"}};
    auto rep2 = http_post(port_, "/", read_req.dump());
    auto j2 = nlohmann::json::parse(rep2.body);
    ASSERT_TRUE(j2["result"].contains("contents"));
    EXPECT_EQ(j2["result"]["contents"][0]["text"], "hello");
}

TEST_F(McpRoundtripTest, PromptsListAndGet) {
    auto rep = http_post(port_, "/",
                         R"({"jsonrpc":"2.0","id":20,"method":"prompts/list"})");
    auto j = nlohmann::json::parse(rep.body);
    EXPECT_EQ(j["result"]["prompts"].size(), 1u);

    auto rep2 = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":21,"method":"prompts/get","params":{"name":"greet","arguments":{"x":1}}})");
    auto j2 = nlohmann::json::parse(rep2.body);
    ASSERT_TRUE(j2["result"].contains("messages"));
    EXPECT_EQ(j2["result"]["messages"][0]["role"], "user");
}

// ---------- Error paths ---------------------------------------------------

TEST_F(McpRoundtripTest, UnknownMethodReturnsMethodNotFound) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":99,"method":"nonexistent/thing"})");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], rpc_error::kMethodNotFound);
}

TEST_F(McpRoundtripTest, ParseErrorReturnsMinus32700) {
    auto rep = http_post(port_, "/", "{not json}");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], rpc_error::kParseError);
}

TEST_F(McpRoundtripTest, MissingToolNameReturnsInvalidParams) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{}})");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], rpc_error::kInvalidParams);
}

// ---------- Resource templates -------------------------------------------

TEST_F(McpRoundtripTest, ResourcesTemplatesListWithProvider) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":30,"method":"resources/templates/list"})");
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
    ASSERT_TRUE(j["result"].contains("resourceTemplates"));
    ASSERT_TRUE(j["result"]["resourceTemplates"].is_array());
    ASSERT_EQ(j["result"]["resourceTemplates"].size(), 1u);
    EXPECT_EQ(j["result"]["resourceTemplates"][0]["uriTemplate"],
              "hermes://sessions/{id}");
}

TEST(ResourcesTemplatesTest, EmptyWhenProviderMissing) {
    // Dispatcher without any resource provider — templates call should
    // still succeed with an empty array (never method_not_found).
    RpcDispatcher::Options o;
    RpcDispatcher d(o);
    auto r = d.method_resources_templates_list();
    ASSERT_TRUE(r.contains("resourceTemplates"));
    EXPECT_TRUE(r["resourceTemplates"].is_array());
    EXPECT_EQ(r["resourceTemplates"].size(), 0u);
}

TEST(ResourcesTemplatesTest, EmptyWhenListTemplatesNotSet) {
    // Resource provider installed but ``list_templates`` not populated.
    RpcDispatcher::Options o;
    o.resources = std::make_shared<ResourceProvider>();
    o.resources->list = [] { return nlohmann::json::array(); };
    RpcDispatcher d(o);
    auto r = d.method_resources_templates_list();
    ASSERT_TRUE(r.contains("resourceTemplates"));
    EXPECT_EQ(r["resourceTemplates"].size(), 0u);
}

// ---------- resources/subscribe + unsubscribe ----------------------------

TEST_F(McpRoundtripTest, ResourcesSubscribeAndUnsubscribeRoundtrip) {
    // Subscribe
    auto sub_rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":40,"method":"resources/subscribe","params":{"uri":"hermes://readme"}})");
    ASSERT_EQ(sub_rep.status, 200);
    auto j = nlohmann::json::parse(sub_rep.body);
    ASSERT_TRUE(j.contains("result"));
    EXPECT_TRUE(j["result"].is_object());

    // Unsubscribe
    auto un_rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":41,"method":"resources/unsubscribe","params":{"uri":"hermes://readme"}})");
    ASSERT_EQ(un_rep.status, 200);
    auto j2 = nlohmann::json::parse(un_rep.body);
    ASSERT_TRUE(j2.contains("result"));
}

TEST_F(McpRoundtripTest, ResourcesSubscribeWithoutUriFails) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":42,"method":"resources/subscribe","params":{}})");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], rpc_error::kInvalidParams);
}

TEST(SubscriptionTest, DispatcherRecordsSubscriptionOnSession) {
    // The POST / one-shot path used above does NOT associate a session, so
    // we exercise the handler directly with a fabricated session.
    RpcDispatcher::Options o;
    o.resources = std::make_shared<ResourceProvider>();
    RpcDispatcher d(o);

    auto session = std::make_shared<McpSession>();
    nlohmann::json sub_params = {{"uri", "hermes://foo"}};
    d.method_resources_subscribe(sub_params, session);
    EXPECT_EQ(session->subscriptions.size(), 1u);
    EXPECT_TRUE(session->subscriptions.count("hermes://foo"));

    d.method_resources_unsubscribe(sub_params, session);
    EXPECT_EQ(session->subscriptions.size(), 0u);
}

// ---------- logging/setLevel ---------------------------------------------

TEST_F(McpRoundtripTest, LoggingSetLevelDebug) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":50,"method":"logging/setLevel","params":{"level":"debug"}})");
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
    EXPECT_TRUE(j["result"].is_object());
}

TEST_F(McpRoundtripTest, LoggingSetLevelWarning) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":51,"method":"logging/setLevel","params":{"level":"warning"}})");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
}

TEST_F(McpRoundtripTest, LoggingSetLevelInvalidReturnsInvalidParams) {
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":52,"method":"logging/setLevel","params":{"level":"bogus"}})");
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], rpc_error::kInvalidParams);
}

TEST(LoggingSinkTest, CustomSinkIsInvoked) {
    RpcDispatcher::Options o;
    std::string captured;
    o.logging_sink = [&](const std::string& l) { captured = l; };
    RpcDispatcher d(o);
    d.method_logging_set_level({{"level", "error"}});
    EXPECT_EQ(captured, "error");
}

// ---------- completion/complete -------------------------------------------

TEST_F(McpRoundtripTest, CompletionCompleteWithoutProviderReturnsEmpty) {
    // Base fixture does not install a completion provider — expect the
    // empty-completion sentinel, NOT method_not_found.
    auto rep = http_post(
        port_, "/",
        R"({"jsonrpc":"2.0","id":60,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"greet"},"argument":{"name":"x","value":""}}})");
    ASSERT_EQ(rep.status, 200);
    auto j = nlohmann::json::parse(rep.body);
    ASSERT_TRUE(j.contains("result"));
    ASSERT_TRUE(j["result"].contains("completion"));
    EXPECT_EQ(j["result"]["completion"]["values"].size(), 0u);
    EXPECT_EQ(j["result"]["completion"]["total"], 0);
    EXPECT_EQ(j["result"]["completion"]["hasMore"], false);
}

TEST(CompletionTest, ProviderResultsAreForwarded) {
    RpcDispatcher::Options o;
    o.completions = std::make_shared<CompletionProvider>();
    o.completions->complete = [](const std::string& ref_type,
                                 const std::string& ref_name,
                                 const std::string& arg_name,
                                 const std::string& arg_value) {
        EXPECT_EQ(ref_type, "ref/prompt");
        EXPECT_EQ(ref_name, "greet");
        EXPECT_EQ(arg_name, "name");
        EXPECT_EQ(arg_value, "al");
        return nlohmann::json::array({"alice", "alex"});
    };
    RpcDispatcher d(o);
    nlohmann::json params = {
        {"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
        {"argument", {{"name", "name"}, {"value", "al"}}},
    };
    auto r = d.method_completion_complete(params);
    ASSERT_TRUE(r.contains("completion"));
    EXPECT_EQ(r["completion"]["values"].size(), 2u);
    EXPECT_EQ(r["completion"]["values"][0], "alice");
    EXPECT_EQ(r["completion"]["total"], 2);
}

TEST(CompletionTest, ResourceRefUriAccepted) {
    RpcDispatcher::Options o;
    o.completions = std::make_shared<CompletionProvider>();
    o.completions->complete = [](const std::string& ref_type,
                                 const std::string& ref_name,
                                 const std::string&, const std::string&) {
        EXPECT_EQ(ref_type, "ref/resource");
        EXPECT_EQ(ref_name, "hermes://xyz");
        nlohmann::json out;
        out["completion"] = {{"values", {"a", "b"}}, {"total", 2}, {"hasMore", false}};
        return out;
    };
    RpcDispatcher d(o);
    nlohmann::json params = {
        {"ref", {{"type", "ref/resource"}, {"uri", "hermes://xyz"}}},
        {"argument", {{"name", "q"}, {"value", ""}}},
    };
    auto r = d.method_completion_complete(params);
    EXPECT_EQ(r["completion"]["values"].size(), 2u);
}

TEST(CompletionTest, BadParamsThrow) {
    RpcDispatcher::Options o;
    RpcDispatcher d(o);
    // Non-object ``argument`` — should throw std::invalid_argument which
    // the dispatcher converts to -32602.
    nlohmann::json bad = {
        {"ref", {{"type", "ref/prompt"}, {"name", "x"}}},
        {"argument", "not-an-object"},
    };
    EXPECT_THROW(d.method_completion_complete(bad), std::invalid_argument);
    // Non-object params entirely.
    EXPECT_THROW(d.method_completion_complete(nlohmann::json::array()),
                 std::invalid_argument);
}

// ---------- send_notification --------------------------------------------

TEST_F(McpRoundtripTest, SendNotificationEnqueuesSseFrame) {
    // Create a session directly so we can inspect its queue without
    // racing an SSE HTTP client.
    auto s = server_->sessions().create();
    nlohmann::json params = {{"uri", "hermes://readme"}};
    EXPECT_TRUE(server_->send_notification(
        s->id, "notifications/resources/updated", params));

    std::string frame;
    ASSERT_TRUE(s->queue->wait_and_pop(frame, std::chrono::milliseconds(100)));
    EXPECT_NE(frame.find("event: message"), std::string::npos);
    EXPECT_NE(frame.find("notifications/resources/updated"),
              std::string::npos);
    EXPECT_NE(frame.find("hermes://readme"), std::string::npos);
}

TEST_F(McpRoundtripTest, SendNotificationUnknownSessionReturnsFalse) {
    EXPECT_FALSE(server_->send_notification("no-such-session", "ping"));
}

TEST_F(McpRoundtripTest, NotifyResourceUpdatedOnlyHitsSubscribers) {
    auto a = server_->sessions().create();
    auto b = server_->sessions().create();
    {
        std::lock_guard<std::mutex> lk(a->sub_mu);
        a->subscriptions.insert("hermes://readme");
    }
    // ``b`` is not subscribed.

    auto n = server_->notify_resource_updated("hermes://readme");
    EXPECT_EQ(n, 1u);

    std::string frame;
    EXPECT_TRUE(a->queue->wait_and_pop(frame, std::chrono::milliseconds(100)));
    EXPECT_FALSE(
        b->queue->wait_and_pop(frame, std::chrono::milliseconds(50)));
}

// ---------- Session GC ----------------------------------------------------

TEST(SessionStoreTest, GcEvictsExpiredSessions) {
    SessionStore store(std::chrono::minutes(0));  // expire immediately
    auto s = store.create();
    EXPECT_EQ(store.size(), 1u);
    // Back-date last_seen to force expiry.
    s->last_seen = std::chrono::system_clock::now() - std::chrono::hours(1);
    auto evicted = store.gc_expired();
    EXPECT_EQ(evicted, 1u);
    EXPECT_EQ(store.size(), 0u);
}

TEST(SessionStoreTest, TouchRefreshesLastSeen) {
    SessionStore store(std::chrono::minutes(30));
    auto s = store.create();
    auto earlier =
        std::chrono::system_clock::now() - std::chrono::minutes(10);
    s->last_seen = earlier;
    auto s2 = store.touch(s->id);
    ASSERT_TRUE(s2);
    EXPECT_GT(s2->last_seen, earlier);
}

TEST(SessionStoreTest, UuidV4Format) {
    SessionStore store;
    auto s = store.create();
    EXPECT_EQ(s->id.size(), 36u);
    EXPECT_EQ(s->id[8], '-');
    EXPECT_EQ(s->id[13], '-');
    EXPECT_EQ(s->id[14], '4');  // version nibble
    EXPECT_EQ(s->id[18], '-');
    EXPECT_EQ(s->id[23], '-');
}

// ---------- SSE endpoint event -------------------------------------------

TEST_F(McpRoundtripTest, SseStreamEmitsEndpointFrame) {
    // Open an SSE connection on its own thread, read until we see an
    // ``event: endpoint`` frame, then tear down.
    std::atomic<bool> got_endpoint{false};
    std::string captured_session_id;

    std::thread client([&] {
        try {
            asio::io_context io;
            tcp::socket sock(io);
            sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                        port_));
            std::string req =
                "GET /sse HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Accept: text/event-stream\r\n"
                "Connection: close\r\n\r\n";
            asio::write(sock, asio::buffer(req));

            std::string buf;
            char tmp[1024];
            boost::system::error_code ec;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                sock.non_blocking(true);
                std::size_t n = sock.read_some(asio::buffer(tmp), ec);
                if (n > 0) buf.append(tmp, n);
                if (buf.find("event: endpoint") != std::string::npos) {
                    got_endpoint.store(true);
                    auto p = buf.find("sessionId=");
                    if (p != std::string::npos) {
                        auto e = buf.find('\n', p);
                        captured_session_id = buf.substr(p + 10, e - p - 10);
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            sock.close(ec);
        } catch (...) {
        }
    });

    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got_endpoint.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (client.joinable()) client.join();

    EXPECT_TRUE(got_endpoint.load());
    EXPECT_FALSE(captured_session_id.empty());
}

}  // namespace
}  // namespace hermes::mcp_server
