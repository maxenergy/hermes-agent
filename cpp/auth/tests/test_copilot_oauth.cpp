#include <gtest/gtest.h>

#include "hermes/auth/copilot_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

using hermes::auth::CopilotOAuth;
using hermes::llm::FakeHttpTransport;

TEST(CopilotOAuth, RequestDeviceCodeParsesResponse) {
    FakeHttpTransport fake;
    fake.enqueue_response({200,
                           R"({"device_code":"ABC123","user_code":"WXYZ-1234",)"
                           R"("verification_uri":"https://github.com/login/device",)"
                           R"("expires_in":900,"interval":5})",
                           {}});
    CopilotOAuth oauth(&fake, "test-client");
    auto dc = oauth.request_device_code();
    EXPECT_EQ(dc.device_code, "ABC123");
    EXPECT_EQ(dc.user_code, "WXYZ-1234");
    EXPECT_EQ(dc.interval, 5);
    EXPECT_EQ(dc.expires_in, 900);
}

TEST(CopilotOAuth, PollForTokenReturnsTokenOn200) {
    FakeHttpTransport fake;
    fake.enqueue_response({200,
                           R"({"access_token":"gho_ABC","token_type":"bearer","scope":"read:user"})",
                           {}});
    CopilotOAuth oauth(&fake, "test-client");
    auto tok = oauth.poll_for_token("devcode", std::chrono::seconds(0),
                                     std::chrono::seconds(1));
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->access_token, "gho_ABC");
    EXPECT_EQ(tok->token_type, "bearer");
}

TEST(CopilotOAuth, PollForTokenHandlesAuthorizationPending) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"error":"authorization_pending"})", {}});
    fake.enqueue_response({200,
                           R"({"access_token":"gho_XYZ","token_type":"bearer"})",
                           {}});
    CopilotOAuth oauth(&fake, "test-client");
    auto tok = oauth.poll_for_token("devcode", std::chrono::seconds(0),
                                     std::chrono::seconds(2));
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->access_token, "gho_XYZ");
}

TEST(CopilotOAuth, PollForTokenReturnsNulloptOnExpired) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"error":"expired_token"})", {}});
    CopilotOAuth oauth(&fake, "test-client");
    auto tok = oauth.poll_for_token("devcode", std::chrono::seconds(0),
                                     std::chrono::seconds(1));
    EXPECT_FALSE(tok.has_value());
}

TEST(CopilotOAuth, GetCopilotTokenExtractsField) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"token":"copilot_token_xyz","expires_at":1700000000})", {}});
    CopilotOAuth oauth(&fake, "test-client");
    auto tok = oauth.get_copilot_token("gh_token");
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(*tok, "copilot_token_xyz");
}
