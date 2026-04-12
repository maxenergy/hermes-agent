#include <gtest/gtest.h>

#include <hermes/gateway/gateway_runner.hpp>

#include <filesystem>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

// Mock adapter for testing.
class MockAdapter : public hg::BasePlatformAdapter {
public:
    explicit MockAdapter(hg::Platform p) : platform_(p) {}

    hg::Platform platform() const override { return platform_; }

    bool connect() override {
        connected = true;
        return true;
    }

    void disconnect() override { connected = false; }

    bool send(const std::string& chat_id,
              const std::string& content) override {
        last_chat_id = chat_id;
        last_content = content;
        return true;
    }

    bool connected = false;
    std::string last_chat_id;
    std::string last_content;

private:
    hg::Platform platform_;
};

class GatewayRunnerTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;
    fs::path original_home_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_runner_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);

        original_home_ = std::getenv("HERMES_HOME")
                             ? std::getenv("HERMES_HOME")
                             : "";
        setenv("HERMES_HOME", tmp_dir_.c_str(), 1);
    }

    void TearDown() override {
        if (original_home_.empty()) {
            unsetenv("HERMES_HOME");
        } else {
            setenv("HERMES_HOME", original_home_.c_str(), 1);
        }
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(GatewayRunnerTest, RegisterAdapter) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;

    hg::GatewayRunner runner(config, &store, &hooks);

    auto adapter = std::make_unique<MockAdapter>(hg::Platform::Telegram);
    auto* raw = adapter.get();
    runner.register_adapter(std::move(adapter));

    // Adapter is registered but not yet connected.
    EXPECT_FALSE(raw->connected);
}

TEST_F(GatewayRunnerTest, StartCallsConnect) {
    hg::GatewayConfig config;
    hg::PlatformConfig pc;
    pc.enabled = true;
    config.platforms[hg::Platform::Telegram] = pc;

    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;

    hg::GatewayRunner runner(config, &store, &hooks);

    auto adapter = std::make_unique<MockAdapter>(hg::Platform::Telegram);
    auto* raw = adapter.get();
    runner.register_adapter(std::move(adapter));

    runner.start();
    EXPECT_TRUE(raw->connected);

    runner.stop();
    EXPECT_FALSE(raw->connected);
}

TEST_F(GatewayRunnerTest, HandleMessageCreatesSession) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;

    hg::GatewayRunner runner(config, &store, &hooks);

    hg::MessageEvent event;
    event.text = "hello";
    event.message_type = "TEXT";
    event.source.platform = hg::Platform::Telegram;
    event.source.chat_id = "chat_1";
    event.source.user_id = "user_1";
    event.source.chat_type = "dm";

    runner.handle_message(event);

    // Session should now exist.
    auto key = store.get_or_create_session(event.source);
    EXPECT_FALSE(key.empty());
}

TEST_F(GatewayRunnerTest, StartEmitsGatewayStartup) {
    hg::GatewayConfig config;
    hg::SessionStore store(tmp_dir_ / "sessions");
    hg::HookRegistry hooks;

    bool startup_emitted = false;
    hooks.register_hook(hg::EVT_GATEWAY_STARTUP,
                        [&](const std::string&, const nlohmann::json&) {
                            startup_emitted = true;
                        });

    hg::GatewayRunner runner(config, &store, &hooks);
    runner.start();

    EXPECT_TRUE(startup_emitted);

    runner.stop();
}
