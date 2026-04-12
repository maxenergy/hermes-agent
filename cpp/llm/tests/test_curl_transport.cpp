// Tests for the real CurlTransport implementation.
#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using hermes::llm::HttpTransport;
using hermes::llm::make_curl_transport;
using hermes::llm::get_default_transport;

// make_curl_transport() must return a non-null pointer (not throw) when
// CURL or cpr was found at configure time.
TEST(CurlTransport, MakeCurlTransportReturnsNonNull) {
    std::unique_ptr<HttpTransport> tp;
    ASSERT_NO_THROW(tp = make_curl_transport());
    EXPECT_NE(tp, nullptr);
}

// get_default_transport() returns the lazy singleton.
TEST(CurlTransport, GetDefaultTransportReturnsNonNull) {
    auto* tp = get_default_transport();
    EXPECT_NE(tp, nullptr);
}

// get_default_transport() returns the same pointer on repeated calls.
TEST(CurlTransport, GetDefaultTransportIsSingleton) {
    auto* a = get_default_transport();
    auto* b = get_default_transport();
    EXPECT_EQ(a, b);
}

// The get() default implementation delegates to post_json via the
// FakeHttpTransport — verify at the interface level.
TEST(CurlTransport, FakeTransportGetDelegatesToPostJson) {
    hermes::llm::FakeHttpTransport fake;
    HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = "ok";
    fake.enqueue_response(resp);

    auto result = fake.get("https://example.com", {{"Accept", "text/plain"}});
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "ok");
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_EQ(fake.requests()[0].url, "https://example.com");
    EXPECT_EQ(fake.requests()[0].body, "");  // GET sends empty body
}

// ── Live HTTP tests (opt-in via LIVE_HTTP_TEST=1 env) ──────────────────

class LiveCurlTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* env = std::getenv("LIVE_HTTP_TEST");
        if (!env || std::string(env) != "1") {
            GTEST_SKIP() << "LIVE_HTTP_TEST=1 not set — skipping live test";
        }
        transport_ = make_curl_transport();
    }
    std::unique_ptr<HttpTransport> transport_;
};

TEST_F(LiveCurlTransportTest, PostJsonToHttpbin) {
    auto resp = transport_->post_json(
        "https://httpbin.org/post",
        {{"Content-Type", "application/json"}},
        R"({"hello":"world"})");
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_FALSE(resp.body.empty());
    EXPECT_NE(resp.body.find("hello"), std::string::npos);
}

TEST_F(LiveCurlTransportTest, GetFromHttpbin) {
    auto resp = transport_->get(
        "https://httpbin.org/get?foo=bar",
        {{"Accept", "application/json"}});
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_FALSE(resp.body.empty());
    EXPECT_NE(resp.body.find("foo"), std::string::npos);
}

TEST_F(LiveCurlTransportTest, Returns404OnNotFound) {
    auto resp = transport_->get(
        "https://httpbin.org/status/404", {});
    EXPECT_EQ(resp.status_code, 404);
}
