// Tests for Phase 12 polish: BotCommand menu, Slack thread routing,
// per-platform scoped locks (whatsapp/signal/matrix).
#include <gtest/gtest.h>

#include "../platforms/matrix.hpp"
#include "../platforms/signal.hpp"
#include "../platforms/slack.hpp"
#include "../platforms/telegram.hpp"
#include "../platforms/whatsapp.hpp"

#include <cstdlib>
#include <filesystem>

#include <hermes/gateway/status.hpp>
#include <hermes/llm/llm_client.hpp>
#include <nlohmann/json.hpp>

using hermes::gateway::platforms::MatrixAdapter;
using hermes::gateway::platforms::SignalAdapter;
using hermes::gateway::platforms::SlackAdapter;
using hermes::gateway::platforms::TelegramAdapter;
using hermes::gateway::platforms::WhatsAppAdapter;
using hermes::llm::FakeHttpTransport;

namespace fs = std::filesystem;

namespace {

class PolishEnv : public ::testing::Test {
protected:
    void SetUp() override {
        lock_dir_ = fs::temp_directory_path() /
                    ("hermes-locks-" + std::to_string(::getpid()) + "-" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(lock_dir_);
        ::setenv("HERMES_GATEWAY_LOCK_DIR", lock_dir_.c_str(), 1);
    }
    void TearDown() override {
        ::unsetenv("HERMES_GATEWAY_LOCK_DIR");
        std::error_code ec;
        fs::remove_all(lock_dir_, ec);
    }
    fs::path lock_dir_;
};

}  // namespace

// ── Telegram setMyCommands ──────────────────────────────────────────────

TEST(TelegramBotCommands, PostsCanonicalList) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true,"result":true})", {}});
    TelegramAdapter::Config cfg;
    cfg.bot_token = "BOT";
    TelegramAdapter adapter(cfg, &fake);

    std::vector<std::pair<std::string, std::string>> cmds = {
        {"new", "Start a new conversation"},
        {"status", "Show status"},
    };
    EXPECT_TRUE(adapter.set_my_commands(cmds));
    ASSERT_EQ(fake.requests().size(), 1u);
    EXPECT_NE(fake.requests()[0].url.find("setMyCommands"), std::string::npos);
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    ASSERT_TRUE(body.contains("commands"));
    ASSERT_EQ(body["commands"].size(), 2u);
    EXPECT_EQ(body["commands"][0]["command"], "new");
    EXPECT_EQ(body["commands"][1]["command"], "status");
}

TEST(TelegramBotCommands, TruncatesDescription) {
    FakeHttpTransport fake;
    fake.enqueue_response({200, R"({"ok":true,"result":true})", {}});
    TelegramAdapter::Config cfg;
    cfg.bot_token = "BOT";
    TelegramAdapter adapter(cfg, &fake);

    std::string huge(400, 'x');
    EXPECT_TRUE(adapter.set_my_commands({{"big", huge}}));
    auto body = nlohmann::json::parse(fake.requests()[0].body);
    EXPECT_EQ(body["commands"][0]["description"].get<std::string>().size(),
              256u);
}

// ── Slack should_handle_event ───────────────────────────────────────────

TEST(SlackShouldHandle, DmAlwaysHandled) {
    nlohmann::json e = {{"channel", "D123"}, {"text", "hey"}};
    EXPECT_TRUE(SlackAdapter::should_handle_event(e, "U_BOT"));
}

TEST(SlackShouldHandle, ThreadReplyHandledWithoutMention) {
    nlohmann::json e = {{"channel", "C123"},
                         {"thread_ts", "1700000000.000100"},
                         {"text", "follow-up"}};
    EXPECT_TRUE(SlackAdapter::should_handle_event(e, "U_BOT"));
}

TEST(SlackShouldHandle, ChannelMessageRequiresMention) {
    nlohmann::json e = {{"channel", "C123"}, {"text", "hello"}};
    EXPECT_FALSE(SlackAdapter::should_handle_event(e, "U_BOT"));
    nlohmann::json e2 = {{"channel", "C123"}, {"text", "hi <@U_BOT>"}};
    EXPECT_TRUE(SlackAdapter::should_handle_event(e2, "U_BOT"));
}

TEST(SlackShouldHandle, IgnoresOwnBotMessages) {
    nlohmann::json e = {{"channel", "C123"},
                         {"thread_ts", "1700000000.000100"},
                         {"bot_id", "B_SELF"},
                         {"text", "echo"}};
    EXPECT_FALSE(SlackAdapter::should_handle_event(e, "U_BOT"));
}

TEST(SlackShouldHandle, IgnoresEditDeleteSubtypes) {
    nlohmann::json e = {{"channel", "D123"},
                         {"subtype", "message_changed"},
                         {"text", "edited"}};
    EXPECT_FALSE(SlackAdapter::should_handle_event(e, "U_BOT"));
}

// ── WhatsApp scoped lock writes a lock file ─────────────────────────────
// The scoped lock is a CROSS-PROCESS guard (per the Python contract):
// same-pid re-acquisition is intentionally idempotent.  We assert here
// that the lock file is created on connect() and removed on disconnect().

namespace {
size_t count_lock_files(const fs::path& dir, const std::string& prefix) {
    size_t n = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().rfind(prefix, 0) == 0) ++n;
    }
    return n;
}
}  // namespace

TEST_F(PolishEnv, WhatsAppWritesLockFile) {
    WhatsAppAdapter::Config cfg;
    cfg.phone = "1234567890";
    WhatsAppAdapter a(cfg);
    EXPECT_TRUE(a.connect());
    EXPECT_EQ(count_lock_files(lock_dir_, "whatsapp-"), 1u);
    a.disconnect();
    EXPECT_EQ(count_lock_files(lock_dir_, "whatsapp-"), 0u);
}

TEST_F(PolishEnv, SignalWritesLockFile) {
    SignalAdapter::Config cfg;
    cfg.http_url = "http://signal.invalid";
    cfg.account = "+15555550100";
    FakeHttpTransport ok_fake;
    ok_fake.enqueue_response({200, "{}", {}});
    SignalAdapter a(cfg, &ok_fake);
    EXPECT_TRUE(a.connect());
    EXPECT_EQ(count_lock_files(lock_dir_, "signal-"), 1u);
    a.disconnect();
    EXPECT_EQ(count_lock_files(lock_dir_, "signal-"), 0u);
}

TEST_F(PolishEnv, MatrixWritesLockFile) {
    MatrixAdapter::Config cfg;
    cfg.homeserver = "https://matrix.invalid";
    cfg.username = "@bot:matrix.invalid";
    cfg.access_token = "syt_xxx";
    FakeHttpTransport fake_a;
    MatrixAdapter a(cfg, &fake_a);
    EXPECT_TRUE(a.connect());
    EXPECT_EQ(count_lock_files(lock_dir_, "matrix-"), 1u);
    a.disconnect();
    EXPECT_EQ(count_lock_files(lock_dir_, "matrix-"), 0u);
}
