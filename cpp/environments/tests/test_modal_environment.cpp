#include "hermes/environments/modal.hpp"
#include "hermes/environments/snapshot_store.hpp"

#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
namespace he = hermes::environments;
using hermes::llm::FakeHttpTransport;

namespace {

fs::path tmp_store(const std::string& leaf) {
    auto dir = fs::temp_directory_path() /
               ("hermes_modal_" + std::to_string(::getpid()) + "_" + leaf);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "snaps.json";
}

hermes::llm::HttpTransport::Response make_resp(int status,
                                               const std::string& body) {
    hermes::llm::HttpTransport::Response r;
    r.status_code = status;
    r.body = body;
    return r;
}

}  // namespace

TEST(ModalEnvironment, ConfigDefaults) {
    he::ModalEnvironment::Config cfg;
    EXPECT_EQ(cfg.app_name, "hermes-agent");
    EXPECT_EQ(cfg.image, "python:3.11-slim");
    EXPECT_DOUBLE_EQ(cfg.cpus, 0.5);
    EXPECT_EQ(cfg.memory, "1Gi");
    EXPECT_EQ(cfg.api_url, "https://api.modal.com");

    he::ModalEnvironment env(cfg);
    EXPECT_EQ(env.name(), "modal");
}

TEST(ModalEnvironment, ExecuteViaFakeTransport) {
    FakeHttpTransport fake;
    // 1) sandbox creation.
    fake.enqueue_response(make_resp(201, R"({"sandbox_id":"sb-42"})"));
    // 2) exec.
    fake.enqueue_response(make_resp(
        200,
        R"({"exit_code":0,"stdout":"hello\n","stderr":""})"));

    he::ModalEnvironment::Config cfg;
    cfg.token_id = "tid";
    cfg.token_secret = "tsec";
    he::ModalEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("echo hello", opts);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "hello\n");
    EXPECT_EQ(env.sandbox_id(), "sb-42");

    ASSERT_GE(fake.requests().size(), 2u);
    EXPECT_NE(fake.requests()[0].url.find("/v1/sandboxes"),
              std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("sb-42/exec"), std::string::npos);
    // Bearer auth header is present.
    EXPECT_EQ(fake.requests()[0].headers.at("Authorization"), "Bearer tsec");
}

TEST(ModalEnvironment, SnapshotReusesSandboxId) {
    auto store = std::make_shared<he::SnapshotStore>(tmp_store("snapshot"));
    store->save("task-1", {{"sandbox_id", "sb-reused"}, {"provider", "modal"}});

    FakeHttpTransport fake;
    // Only the exec call — creation must be skipped thanks to snapshot.
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"ok\n","stderr":""})"));

    he::ModalEnvironment::Config cfg;
    cfg.token_secret = "tsec";
    cfg.task_id = "task-1";
    he::ModalEnvironment env(cfg, &fake, store);

    he::ExecuteOptions opts;
    auto result = env.execute("pwd", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(env.sandbox_id(), "sb-reused");
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("sb-reused/exec"),
              std::string::npos);
}

TEST(ModalEnvironment, CleanupPostsTerminateAndClearsSnapshot) {
    auto store = std::make_shared<he::SnapshotStore>(tmp_store("cleanup"));

    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(201, R"({"sandbox_id":"sb-99"})"));
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"","stderr":""})"));
    fake.enqueue_response(make_resp(200, "{}"));  // terminate

    he::ModalEnvironment::Config cfg;
    cfg.token_secret = "tsec";
    cfg.task_id = "task-c";
    he::ModalEnvironment env(cfg, &fake, store);

    he::ExecuteOptions opts;
    env.execute("true", opts);

    ASSERT_TRUE(store->load("task-c").has_value());

    env.cleanup();
    // Terminate request was made.
    ASSERT_EQ(fake.requests().size(), 3u);
    EXPECT_NE(fake.requests()[2].url.find("sb-99/terminate"),
              std::string::npos);
    // Snapshot removed.
    EXPECT_FALSE(store->load("task-c").has_value());
}

TEST(ModalEnvironment, HttpFailureReturnsExitOne) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(500, R"({"error":"boom"})"));

    he::ModalEnvironment::Config cfg;
    cfg.token_secret = "tsec";
    he::ModalEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("true", opts);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.stderr_text.find("500"), std::string::npos);
}
