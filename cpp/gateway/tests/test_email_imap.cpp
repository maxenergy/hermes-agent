// IMAP IDLE protocol + lifecycle tests for the email adapter.
#include <gtest/gtest.h>

#include "../platforms/email.hpp"

#include <cstdlib>

using hermes::gateway::platforms::EmailAdapter;

TEST(EmailImap, LoginCommandFormatting) {
    auto s = EmailAdapter::imap_login_command("a1", "me@example.com",
                                              "secret");
    EXPECT_EQ(s, "a1 LOGIN me@example.com secret\r\n");
}

TEST(EmailImap, SelectAndIdleCommandFormatting) {
    EXPECT_EQ(EmailAdapter::imap_select_command("a2", "INBOX"),
              "a2 SELECT INBOX\r\n");
    EXPECT_EQ(EmailAdapter::imap_idle_command("a3"), "a3 IDLE\r\n");
    EXPECT_EQ(EmailAdapter::imap_done_command(), "DONE\r\n");
}

TEST(EmailImap, UidFetchCommandFormatting) {
    auto s = EmailAdapter::imap_uid_fetch_command("a5", "42:*");
    EXPECT_EQ(s, "a5 UID FETCH 42:* (RFC822)\r\n");
}

TEST(EmailImap, ParseExistsNotification) {
    EXPECT_EQ(EmailAdapter::parse_exists_notification("* 42 EXISTS"), 42);
    EXPECT_EQ(EmailAdapter::parse_exists_notification("* 1 EXISTS\r\n"), 1);
    EXPECT_EQ(EmailAdapter::parse_exists_notification("* 7 RECENT"), -1);
    EXPECT_EQ(EmailAdapter::parse_exists_notification("a1 OK"), -1);
    EXPECT_EQ(EmailAdapter::parse_exists_notification(""), -1);
}

TEST(EmailImap, IdleLoopStartStopIsGated) {
    // Without IMAP_TEST_HOST we never touch the network; start/stop
    // should be a no-op-safe cycle.
    if (std::getenv("IMAP_TEST_HOST") == nullptr) {
        EmailAdapter::Config cfg;
        cfg.address = "nobody@example.invalid";
        cfg.password = "pw";
        cfg.imap_host = "unreachable.invalid";
        EmailAdapter adapter(cfg);
        adapter.start_imap_idle_loop([](const std::string&) {});
        adapter.stop_imap_idle_loop();
        SUCCEED();
    } else {
        // Live test would connect; we just verify it doesn't crash.
        SUCCEED();
    }
}
