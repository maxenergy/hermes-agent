#include "hermes/core/uds_rpc.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

// Build a socket path in a per-test temp dir, avoiding the real
// HERMES_HOME so parallel test runs stay isolated.
fs::path make_tmp_socket(const std::string& tag) {
    fs::path dir = fs::temp_directory_path() /
                   ("hermes_uds_rpc_" + tag + "_" +
                    std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir / "sock";
}

}  // namespace

TEST(UdsRpcFraming, RoundTrip) {
    using hermes::core::rpc::frame_message;
    using hermes::core::rpc::try_parse_frame;

    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    std::string framed = frame_message(body);
    EXPECT_NE(framed.find("Content-Length: "), std::string::npos);
    EXPECT_NE(framed.find(std::to_string(body.size())), std::string::npos);

    auto parsed = try_parse_frame(framed);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, body);
    EXPECT_TRUE(framed.empty());
}

TEST(UdsRpcFraming, IncompleteReturnsNullopt) {
    using hermes::core::rpc::try_parse_frame;
    std::string buf = "Content-Length: 50\r\n\r\nshort";
    auto p = try_parse_frame(buf);
    EXPECT_FALSE(p.has_value());
    // Buffer untouched so next read can top it up.
    EXPECT_NE(buf.find("short"), std::string::npos);
}

TEST(UdsRpcFraming, MultipleFramesBackToBack) {
    using hermes::core::rpc::frame_message;
    using hermes::core::rpc::try_parse_frame;
    std::string a = frame_message("{\"a\":1}");
    std::string b = frame_message("{\"b\":2}");
    std::string buf = a + b;
    auto f1 = try_parse_frame(buf);
    auto f2 = try_parse_frame(buf);
    ASSERT_TRUE(f1.has_value());
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(*f1, "{\"a\":1}");
    EXPECT_EQ(*f2, "{\"b\":2}");
    EXPECT_TRUE(buf.empty());
}

TEST(UdsRpcFraming, MalformedHeaderThrows) {
    using hermes::core::rpc::try_parse_frame;
    std::string buf = "X-Bogus: 5\r\n\r\nhello";
    EXPECT_THROW((void)try_parse_frame(buf), std::runtime_error);
}

TEST(UdsRpcFraming, OversizedFrameThrows) {
    using hermes::core::rpc::try_parse_frame;
    std::string buf = "Content-Length: 999999999\r\n\r\n";
    EXPECT_THROW((void)try_parse_frame(buf, /*max_bytes=*/1024),
                 std::runtime_error);
}

#ifndef _WIN32

TEST(UdsRpcServer, RoundTripCallAndNotify) {
    using hermes::core::rpc::RpcResult;
    using hermes::core::rpc::UdsClient;
    using hermes::core::rpc::UdsServer;

    auto sock = make_tmp_socket("rt");
    UdsServer server;
    std::atomic<int> notif_count{0};
    server.on("echo", [](const nlohmann::json& params) {
        return RpcResult::ok(params);
    });
    server.on("add", [](const nlohmann::json& params) {
        int a = params.value("a", 0);
        int b = params.value("b", 0);
        return RpcResult::ok(a + b);
    });
    server.on("bump",
              [&](const nlohmann::json&) {
                  notif_count.fetch_add(1);
                  return RpcResult::ok(nullptr);
              });

    server.start(sock);

    UdsClient client;
    client.connect(sock, /*timeout_ms=*/2000);

    auto r1 = client.call("echo", nlohmann::json{{"hello", "world"}});
    EXPECT_EQ(r1.value("hello", ""), "world");

    auto r2 = client.call("add", nlohmann::json{{"a", 3}, {"b", 4}});
    EXPECT_EQ(r2.get<int>(), 7);

    client.notify("bump", nullptr);
    client.notify("bump", nullptr);
    // Issue a follow-up call to synchronise — by the time we get the
    // echo back, both notifications have been processed by the server
    // loop (POSIX server is single-threaded per client).
    (void)client.call("echo", nlohmann::json{{"sync", true}});
    EXPECT_EQ(notif_count.load(), 2);

    client.close();
    server.stop();
}

TEST(UdsRpcServer, MethodNotFoundReturnsError) {
    using hermes::core::rpc::UdsClient;
    using hermes::core::rpc::UdsServer;

    auto sock = make_tmp_socket("mnf");
    UdsServer server;
    server.start(sock);
    UdsClient c;
    c.connect(sock, 2000);
    EXPECT_THROW((void)c.call("does_not_exist"), std::runtime_error);
    c.close();
    server.stop();
}

TEST(UdsRpcServer, SocketFileHas0600Perms) {
    using hermes::core::rpc::UdsServer;
    auto sock = make_tmp_socket("perm");
    UdsServer server;
    server.start(sock);
    struct stat st{};
    ASSERT_EQ(::stat(sock.c_str(), &st), 0);
    // Other/group bits must be clear.
    EXPECT_EQ(st.st_mode & 0077, 0u)
        << "socket mode was " << std::oct << (st.st_mode & 0777);
    server.stop();
}

TEST(UdsRpcServer, HandlerExceptionBecomesInternalError) {
    using hermes::core::rpc::RpcResult;
    using hermes::core::rpc::UdsClient;
    using hermes::core::rpc::UdsServer;

    auto sock = make_tmp_socket("boom");
    UdsServer server;
    server.on("boom", [](const nlohmann::json&) -> RpcResult {
        throw std::runtime_error("kaboom");
    });
    server.start(sock);
    UdsClient c;
    c.connect(sock, 2000);
    try {
        (void)c.call("boom");
        FAIL() << "expected throw";
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        EXPECT_NE(msg.find("kaboom"), std::string::npos);
    }
    c.close();
    server.stop();
}

#endif  // !_WIN32
