// Joint integration tests — QwenOAuth + QwenCredentialStore + QwenClient.

#include "hermes/auth/qwen_client.hpp"
#include "hermes/auth/qwen_oauth.hpp"
#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using hermes::auth::QwenClient;
using hermes::auth::QwenCredentials;
using hermes::auth::QwenCredentialStore;
using hermes::auth::QwenOAuth;
using hermes::llm::CompletionRequest;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::Message;
using hermes::llm::Role;
using json = nlohmann::json;

namespace {

fs::path unique_tmp(const std::string& tag) {
    return fs::temp_directory_path() /
           ("hermes_qwen_joint_" + tag + "_" +
            std::to_string(std::chrono::system_clock::now()
                               .time_since_epoch()
                               .count()));
}

}  // namespace

// 1. Device-code flow end-to-end — request code, poll once, token returned.
TEST(JointQwenPipeline, DeviceCodeFlowEndToEnd) {
    FakeHttpTransport fake;
    // device/code endpoint.
    fake.enqueue_response({200,
        R"({"device_code":"DEV123","user_code":"USER-42",)"
        R"("verification_uri":"https://chat.qwen.ai/authorize",)"
        R"("verification_uri_complete":"https://chat.qwen.ai/authorize?user_code=USER-42",)"
        R"("expires_in":900})",
        {}});
    // token endpoint (poll #1 succeeds).
    fake.enqueue_response({200,
        R"({"access_token":"AT_new","refresh_token":"RT_new",)"
        R"("token_type":"Bearer","resource_url":"portal.qwen.ai",)"
        R"("expires_in":3600})",
        {}});

    QwenOAuth oauth(&fake);
    auto pkce = QwenOAuth::generate_pkce();
    auto dc = oauth.request_device_code(pkce.challenge_s256);
    EXPECT_EQ(dc.device_code, "DEV123");
    EXPECT_EQ(dc.user_code, "USER-42");

    auto tok = oauth.poll_for_token(dc.device_code, pkce.verifier,
                                    std::chrono::seconds(0),
                                    std::chrono::seconds(5));
    ASSERT_TRUE(tok.has_value());
    EXPECT_EQ(tok->access_token, "AT_new");
    EXPECT_EQ(tok->refresh_token, "RT_new");
    EXPECT_EQ(tok->resource_url, "portal.qwen.ai");
    EXPECT_GT(tok->expiry_date_ms, 0);
}

// 2. ensure_valid auto-refreshes when token is expiring, persists new creds.
TEST(JointQwenPipeline, AutoRefreshOnExpiringToken) {
    auto dir = unique_tmp("refresh");
    fs::create_directories(dir);
    auto creds_path = dir / "oauth_creds.json";

    // Pre-seed expired creds with a refresh_token.
    QwenCredentials old;
    old.access_token = "AT_old";
    old.refresh_token = "RT_persist";
    old.resource_url = "portal.qwen.ai";
    old.expiry_date_ms = 1;  // way in the past
    QwenCredentialStore store(creds_path);
    ASSERT_TRUE(store.save(old));

    FakeHttpTransport fake;
    // Refresh call returns a new access token (refresh_token rotated).
    fake.enqueue_response({200,
        R"({"access_token":"AT_rotated","refresh_token":"RT_rotated",)"
        R"("token_type":"Bearer","resource_url":"portal.qwen.ai",)"
        R"("expires_in":3600})",
        {}});

    QwenOAuth oauth(&fake);
    auto fresh = oauth.ensure_valid(store);
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->access_token, "AT_rotated");
    EXPECT_EQ(fresh->refresh_token, "RT_rotated");

    // Store was updated on disk.
    auto reloaded = store.load();
    EXPECT_EQ(reloaded.access_token, "AT_rotated");
    EXPECT_EQ(reloaded.refresh_token, "RT_rotated");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 3. QwenClient request body contains every Qwen-specific required field.
TEST(JointQwenPipeline, ChatCompletionRequestBodyHasRequiredFields) {
    auto dir = unique_tmp("body");
    fs::create_directories(dir);
    auto creds_path = dir / "oauth_creds.json";

    QwenCredentials c;
    c.access_token = "AT_valid";
    c.refresh_token = "RT_valid";
    c.resource_url = "portal.qwen.ai";
    c.expiry_date_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count() +
                       60'000'000;  // not expiring soon
    QwenCredentialStore store(creds_path);
    ASSERT_TRUE(store.save(c));

    FakeHttpTransport fake;
    // Stream response: a single SSE text chunk, then usage + [DONE].
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"hi\"}}]}\n"
        "data: {\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{}}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":1}}\n"
        "data: [DONE]\n";
    fake.enqueue_stream_response(sse);

    QwenClient client(&fake, QwenCredentialStore(creds_path));
    CompletionRequest req;
    req.model = "whatever";
    Message user;
    user.role = Role::User;
    user.content_text = "hello";
    req.messages.push_back(user);

    auto resp = client.complete(req);
    EXPECT_EQ(resp.assistant_message.content_text, "hi");

    ASSERT_FALSE(fake.requests().empty());
    const auto& sent = fake.requests().back().body;
    auto body = json::parse(sent);

    // Required Qwen fields:
    EXPECT_EQ(body.value("model", ""), "coder-model");
    ASSERT_TRUE(body.contains("messages"));
    ASSERT_TRUE(body["messages"].is_array());
    // All message .content blocks must be arrays of {type,text}.
    for (const auto& m : body["messages"]) {
        ASSERT_TRUE(m.contains("content"));
        ASSERT_TRUE(m["content"].is_array());
        ASSERT_FALSE(m["content"].empty());
        EXPECT_TRUE(m["content"][0].contains("type"));
        EXPECT_TRUE(m["content"][0].contains("text"));
    }
    // System message synthesised.
    bool has_system = false;
    for (const auto& m : body["messages"]) {
        if (m.value("role", "") == "system") has_system = true;
    }
    EXPECT_TRUE(has_system);

    // tools must be a non-empty array.
    ASSERT_TRUE(body.contains("tools"));
    ASSERT_TRUE(body["tools"].is_array());
    EXPECT_FALSE(body["tools"].empty());

    // metadata.{sessionId,promptId}
    ASSERT_TRUE(body.contains("metadata"));
    EXPECT_TRUE(body["metadata"].contains("sessionId"));
    EXPECT_TRUE(body["metadata"].contains("promptId"));

    // vl_high_resolution_images flag required by portal.qwen.ai.
    ASSERT_TRUE(body.contains("vl_high_resolution_images"));
    EXPECT_TRUE(body["vl_high_resolution_images"].get<bool>());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 4. Credentials file is byte-compatible with qwen-code's JSON format.
TEST(JointQwenPipeline, CredentialsFileByteCompatibleWithQwenCode) {
    auto dir = unique_tmp("compat");
    fs::create_directories(dir);
    auto creds_path = dir / "oauth_creds.json";

    QwenCredentials c;
    c.access_token = "AT_abc";
    c.refresh_token = "RT_xyz";
    c.token_type = "Bearer";
    c.resource_url = "portal.qwen.ai";
    c.expiry_date_ms = 1'700'000'000'000LL;

    QwenCredentialStore store(creds_path);
    ASSERT_TRUE(store.save(c));

    std::ifstream in(creds_path);
    ASSERT_TRUE(in.good());
    json j;
    in >> j;

    // All keys qwen-code (Node.js CLI) expects.
    EXPECT_TRUE(j.contains("access_token"));
    EXPECT_TRUE(j.contains("refresh_token"));
    EXPECT_TRUE(j.contains("token_type"));
    EXPECT_TRUE(j.contains("resource_url"));
    EXPECT_TRUE(j.contains("expiry_date"));

    EXPECT_EQ(j["access_token"].get<std::string>(), "AT_abc");
    EXPECT_EQ(j["refresh_token"].get<std::string>(), "RT_xyz");
    EXPECT_EQ(j["token_type"].get<std::string>(), "Bearer");
    EXPECT_EQ(j["resource_url"].get<std::string>(), "portal.qwen.ai");
    // qwen-code stores expiry_date as a number (epoch ms).
    EXPECT_TRUE(j["expiry_date"].is_number());
    EXPECT_EQ(j["expiry_date"].get<int64_t>(), 1'700'000'000'000LL);

    // Round-trip loads correctly.
    QwenCredentialStore store2(creds_path);
    auto loaded = store2.load();
    EXPECT_EQ(loaded.access_token, "AT_abc");
    EXPECT_EQ(loaded.refresh_token, "RT_xyz");
    EXPECT_EQ(loaded.expiry_date_ms, 1'700'000'000'000LL);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
