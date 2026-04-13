// Tests for the SSRF-guarded HTTP wrapper.
#include <gtest/gtest.h>

#include <hermes/gateway/safe_fetch.hpp>
#include <hermes/llm/llm_client.hpp>

using hermes::llm::FakeHttpTransport;

TEST(SafeFetch, GetBlocksLoopback) {
    FakeHttpTransport fake;
    auto r = hermes::gateway::safe_get("http://127.0.0.1/x", {}, &fake);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ssrf"), std::string::npos);
    EXPECT_TRUE(fake.requests().empty());
}

TEST(SafeFetch, GetBlocksMetadata) {
    FakeHttpTransport fake;
    auto r = hermes::gateway::safe_get("http://169.254.169.254/latest/", {},
                                        &fake);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ssrf"), std::string::npos);
}

TEST(SafeFetch, GetAllowsPublicHost) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, "OK", {}});
    auto r = hermes::gateway::safe_get("https://example.com/", {}, &fake);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.body, "OK");
    EXPECT_EQ(fake.requests().size(), 1u);
}

TEST(SafeFetch, PostJsonBlocksPrivate) {
    FakeHttpTransport fake;
    auto r = hermes::gateway::safe_post_json("http://10.0.0.1/api", {},
                                               "{}", &fake);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("ssrf"), std::string::npos);
    EXPECT_TRUE(fake.requests().empty());
}

TEST(SafeFetch, PostJsonAllowsPublic) {
    FakeHttpTransport fake;
    fake.enqueue_response({201, R"({"id":1})", {}});
    auto r = hermes::gateway::safe_post_json("https://api.example.com/v1",
                                               {{"X-Test", "y"}}, "{}", &fake);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.status_code, 201);
}
