#include "hermes/environments/managed_modal.hpp"
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
               ("hermes_mm_" + std::to_string(::getpid()) + "_" + leaf);
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

TEST(ManagedModalEnvironment, ConfigDefaults) {
    he::ManagedModalEnvironment::Config cfg;
    EXPECT_EQ(cfg.gateway_url, "https://tool-gateway.nousresearch.com");
    EXPECT_EQ(cfg.connect_timeout.count(), 1);
    EXPECT_EQ(cfg.poll_interval.count(), 5);

    he::ManagedModalEnvironment env(cfg);
    EXPECT_EQ(env.name(), "managed_modal");
}

TEST(ManagedModalEnvironment, ExecuteViaGateway) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(201, R"({"sandbox_id":"sb-mm"})"));
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"ok","stderr":""})"));

    he::ManagedModalEnvironment::Config cfg;
    cfg.api_token = "gateway-token";
    he::ManagedModalEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("echo ok", opts);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.stdout_text, "ok");
    EXPECT_EQ(env.sandbox_id(), "sb-mm");

    ASSERT_GE(fake.requests().size(), 2u);
    EXPECT_NE(fake.requests()[0].url.find("/modal/sandbox/create"),
              std::string::npos);
    EXPECT_NE(fake.requests()[1].url.find("/modal/sandbox/exec"),
              std::string::npos);
    EXPECT_EQ(fake.requests()[0].headers.at("Authorization"),
              "Bearer gateway-token");
}

TEST(ManagedModalEnvironment, CleanupTerminatesAndClearsSnapshot) {
    auto store = std::make_shared<he::SnapshotStore>(tmp_store("cleanup"));

    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(201, R"({"sandbox_id":"sb-c"})"));
    fake.enqueue_response(make_resp(
        200, R"({"exit_code":0,"stdout":"","stderr":""})"));
    fake.enqueue_response(make_resp(200, "{}"));

    he::ManagedModalEnvironment::Config cfg;
    cfg.api_token = "tok";
    cfg.task_id = "t-mm";
    he::ManagedModalEnvironment env(cfg, &fake, store);

    he::ExecuteOptions opts;
    env.execute("true", opts);
    ASSERT_TRUE(store->load("t-mm").has_value());

    env.cleanup();
    ASSERT_EQ(fake.requests().size(), 3u);
    EXPECT_NE(fake.requests()[2].url.find("/modal/sandbox/terminate"),
              std::string::npos);
    EXPECT_FALSE(store->load("t-mm").has_value());
}

TEST(ManagedModalEnvironment, HttpFailureReturnsExitOne) {
    FakeHttpTransport fake;
    fake.enqueue_response(make_resp(502, R"({"error":"bad gateway"})"));

    he::ManagedModalEnvironment::Config cfg;
    cfg.api_token = "tok";
    he::ManagedModalEnvironment env(cfg, &fake, nullptr);

    he::ExecuteOptions opts;
    auto result = env.execute("true", opts);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.stderr_text.find("502"), std::string::npos);
}
