#include "hermes/environments/daytona.hpp"
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
               ("hermes_daytona_" + std::to_string(::getpid()) + "_" + leaf);
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

TEST(DaytonaEnvironment, ConfigDefaults) {
    he::DaytonaEnvironment::Config cfg;
    EXPECT_EQ(cfg.api_url, "https://app.daytona.io/api");
    EXPECT_EQ(cfg.image, "ubuntu:24.04");
    EXPECT_EQ(cfg.cpus, 1);
    EXPECT_EQ(cfg.memory_gib, 2);
    EXPECT_EQ(cfg.disk_gib, 10);

    he::DaytonaEnvironment env(cfg);
    EXPECT_EQ(env.name(), "daytona");
}

TEST(DaytonaEnvironment, ExecuteViaFakeTransport) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(201, R"({"id":"ws-7"})"));
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"hi","stderr":""})"));

    he::DaytonaEnvironment::Config cfg;
    cfg.api_token = "daytok";
    he::DaytonaEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("echo hi", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "hi");
    EXPECT_EQ(env.workspace_id(), "ws-7");

    ASSERT_GE(fake.requests().size(), 2u);
    EXPECT_EQ(fake.requests()[0].headers.at("Authorization"),
              "Bearer daytok");
    EXPECT_NE(fake.requests()[1].url.find("ws-7/exec"), std::string::npos);
}

TEST(DaytonaEnvironment, SnapshotReusesWorkspaceId) {
    auto store = std::make_shared<he::SnapshotStore>(tmp_store("reuse"));
    store->save("t1", {{"workspace_id", "ws-keep"}, {"provider", "daytona"}});

    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"","stderr":""})"));

    he::DaytonaEnvironment::Config cfg;
    cfg.api_token = "tok";
    cfg.task_id = "t1";
    he::DaytonaEnvironment env(cfg, &fake, store);

    he::ExecuteOptions opts;
    auto result = env.execute("true", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(env.workspace_id(), "ws-keep");
    ASSERT_EQ(fake.requests().size(), 1u);
}

TEST(DaytonaEnvironment, CleanupDeletesAndClears) {
    auto store = std::make_shared<he::SnapshotStore>(tmp_store("cleanup"));

    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(201, R"({"id":"ws-x"})"));
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"","stderr":""})"));
    fake.enqueue_response(make_resp(200, "{}"));

    he::DaytonaEnvironment::Config cfg;
    cfg.api_token = "tok";
    cfg.task_id = "t-c";
    he::DaytonaEnvironment env(cfg, &fake, store);

    he::ExecuteOptions opts;
    env.execute("true", opts);
    ASSERT_TRUE(store->load("t-c").has_value());

    env.cleanup();
    ASSERT_EQ(fake.requests().size(), 3u);
    EXPECT_NE(fake.requests()[2].url.find("ws-x/delete"),
              std::string::npos);
    EXPECT_FALSE(store->load("t-c").has_value());
}

TEST(DaytonaEnvironment, HttpFailureReturnsExitOne) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(403, R"({"error":"forbidden"})"));

    he::DaytonaEnvironment::Config cfg;
    cfg.api_token = "tok";
    he::DaytonaEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("true", opts);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.stderr_text.find("403"), std::string::npos);
}
