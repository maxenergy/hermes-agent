#include <gtest/gtest.h>

#include <hermes/gateway/gateway_runner.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class GatewayLifecycleTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;
    std::string original_home_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_lifecycle_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);

        const char* h = std::getenv("HERMES_HOME");
        original_home_ = h ? h : "";
        setenv("HERMES_HOME", tmp_dir_.c_str(), 1);
    }

    void TearDown() override {
        if (original_home_.empty()) {
            unsetenv("HERMES_HOME");
        } else {
            setenv("HERMES_HOME", original_home_.c_str(), 1);
        }
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }
};

TEST_F(GatewayLifecycleTest, GetOrCreateAgentReturnsSameAgentForSameKey) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    // Factory returns nullptr — we're testing the cache layer, not agent
    // construction.  get_or_create_agent stores and returns the same
    // (possibly null) value per session key.
    int factory_calls = 0;
    runner.set_agent_factory(
        [&](const std::string&) -> std::shared_ptr<hermes::agent::AIAgent> {
            ++factory_calls;
            return nullptr;
        });

    auto a1 = runner.get_or_create_agent("session_X");
    auto a2 = runner.get_or_create_agent("session_X");
    EXPECT_EQ(a1, a2);
    // Factory invoked at most once for the same key (the cache entry is
    // stored even for nullptr results).
    EXPECT_EQ(factory_calls, 1);

    auto a3 = runner.get_or_create_agent("session_Y");
    (void)a3;
    EXPECT_EQ(factory_calls, 2);
}

TEST_F(GatewayLifecycleTest, PendingQueueStoresMessages) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    hg::MessageEvent e1;
    e1.text = "first";
    e1.message_type = "TEXT";
    e1.source.platform = hg::Platform::Telegram;
    e1.source.chat_id = "c1";
    e1.source.user_id = "u1";

    hg::MessageEvent e2 = e1;
    e2.text = "second";

    runner.handle_busy_session("session_A", e1);
    runner.handle_busy_session("session_A", e2);

    auto pending = runner.drain_pending("session_A");
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0].text, "first");
    EXPECT_EQ(pending[1].text, "second");

    // Second drain should be empty.
    auto again = runner.drain_pending("session_A");
    EXPECT_TRUE(again.empty());
}

TEST_F(GatewayLifecycleTest, EvictStaleAgentsPreservesFreshAgents) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    // Mark a fresh agent as running.
    runner.mark_agent_running("session_fresh", nullptr);
    EXPECT_TRUE(runner.is_agent_running("session_fresh"));

    // Immediately evict — fresh agents (< STALE_AGENT_THRESHOLD) must remain.
    runner.evict_stale_agents();
    EXPECT_TRUE(runner.is_agent_running("session_fresh"));

    // Explicit finish removes it.
    runner.mark_agent_finished("session_fresh");
    EXPECT_FALSE(runner.is_agent_running("session_fresh"));

    // Evict on empty is a no-op.
    EXPECT_NO_THROW(runner.evict_stale_agents());
}

TEST_F(GatewayLifecycleTest, StaleAgentThresholdConstant) {
    // Verify the stale threshold is the documented 30 minutes.
    constexpr auto threshold = hg::GatewayRunner::STALE_AGENT_THRESHOLD;
    EXPECT_EQ(threshold, std::chrono::minutes(30));
}

TEST_F(GatewayLifecycleTest, HookDiscoveryFromTmpDir) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    // Lay out: <hooks_dir>/my_hook/HOOK.yaml
    auto hooks_dir = tmp_dir_ / "hooks";
    fs::create_directories(hooks_dir / "my_hook");
    {
        std::ofstream ofs(hooks_dir / "my_hook" / "HOOK.yaml");
        ofs << R"({"name": "my_hook", "events": ["session:start", "agent:end"]})";
    }

    // A directory without HOOK.yaml must be ignored silently.
    fs::create_directories(hooks_dir / "not_a_hook");

    size_t before = hooks.size();
    runner.discover_hooks(hooks_dir);
    size_t after = hooks.size();

    // Two registrations (session:start + agent:end) from the one hook.
    EXPECT_EQ(after - before, 2u);
}

TEST_F(GatewayLifecycleTest, HookDiscoveryNonExistentDirIsNoOp) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    size_t before = hooks.size();
    runner.discover_hooks(tmp_dir_ / "does_not_exist");
    EXPECT_EQ(hooks.size(), before);
}

TEST_F(GatewayLifecycleTest, BootMdHookExecutesOnStartup) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;
    hg::GatewayRunner runner(config, &store, &hooks);

    auto boot_md = tmp_dir_ / "BOOT.md";
    {
        std::ofstream ofs(boot_md);
        ofs << "Welcome to the gateway.";
    }

    runner.register_boot_md_hook(boot_md);

    // Emitting gateway:startup should invoke the boot_md hook, which
    // appends a message to session "__boot__".
    hooks.emit(hg::EVT_GATEWAY_STARTUP);

    // The session store should now have the __boot__ session with our
    // content appended.
    auto messages = store.load_transcript("__boot__");
    ASSERT_FALSE(messages.empty());
    // Look for the boot content in any of the appended messages.
    bool found = false;
    for (const auto& m : messages) {
        if (m.contains("content") &&
            m["content"].get<std::string>().find("Welcome to the gateway.") !=
                std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
