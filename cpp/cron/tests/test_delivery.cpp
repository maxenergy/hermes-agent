#include <hermes/cron/delivery.hpp>
#include <hermes/core/atomic_io.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

using namespace hermes::cron;
namespace fs = std::filesystem;

TEST(DeliveryParser, Origin) {
    auto dt = parse_delivery_target("origin");
    EXPECT_EQ(dt.platform, "origin");
    EXPECT_TRUE(dt.is_origin);
    EXPECT_TRUE(dt.chat_id.empty());
    EXPECT_TRUE(dt.thread_id.empty());
}

TEST(DeliveryParser, TelegramWithChatAndThread) {
    auto dt = parse_delivery_target("telegram:123456:789");
    EXPECT_EQ(dt.platform, "telegram");
    EXPECT_EQ(dt.chat_id, "123456");
    EXPECT_EQ(dt.thread_id, "789");
    EXPECT_FALSE(dt.is_origin);
    EXPECT_TRUE(dt.is_explicit);
}

TEST(DeliveryParser, Local) {
    auto dt = parse_delivery_target("local");
    EXPECT_EQ(dt.platform, "local");
    EXPECT_FALSE(dt.is_origin);
    EXPECT_TRUE(dt.is_explicit);
}

TEST(DeliveryParser, TelegramHomeChannel) {
    auto dt = parse_delivery_target("telegram");
    EXPECT_EQ(dt.platform, "telegram");
    EXPECT_TRUE(dt.chat_id.empty());
    EXPECT_TRUE(dt.thread_id.empty());
    EXPECT_TRUE(dt.is_explicit);
}

TEST(DeliveryRouter, LocalWritesFile) {
    // Set HOME to temp dir for isolation.
    auto tmp = fs::temp_directory_path() / "hermes_delivery_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Temporarily override HOME.
    std::string old_home;
    const char* h = std::getenv("HOME");
    if (h) old_home = h;
    setenv("HOME", tmp.c_str(), 1);

    DeliveryRouter router;
    std::vector<DeliveryTarget> targets = {parse_delivery_target("local")};
    router.deliver("hello output", targets, "job-123", "run-456");

    auto expected_file =
        tmp / ".hermes" / "cron" / "output" / "job-123" / "run-456.txt";
    ASSERT_TRUE(fs::exists(expected_file));
    auto content = hermes::core::atomic_io::atomic_read(expected_file);
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "hello output");

    // Restore HOME.
    if (!old_home.empty()) {
        setenv("HOME", old_home.c_str(), 1);
    }
    fs::remove_all(tmp);
}

TEST(DeliveryRouter, PlatformTargetThrows) {
    DeliveryRouter router;
    std::vector<DeliveryTarget> targets = {
        parse_delivery_target("telegram:123")};
    EXPECT_THROW(
        router.deliver("test", targets, "job-1", "run-1"),
        std::runtime_error);
}
