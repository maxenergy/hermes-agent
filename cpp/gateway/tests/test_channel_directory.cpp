#include <gtest/gtest.h>

#include <hermes/gateway/channel_directory.hpp>

namespace hg = hermes::gateway;

TEST(ChannelDirectoryTest, RegisterAndResolve) {
    hg::ChannelDirectory dir;
    dir.register_channel({"chat123", "general", hg::Platform::Telegram});

    auto result = dir.resolve_by_name("general");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, "chat123");
    EXPECT_EQ(result->name, "general");
    EXPECT_EQ(result->platform, hg::Platform::Telegram);
}

TEST(ChannelDirectoryTest, UnknownReturnsNullopt) {
    hg::ChannelDirectory dir;
    dir.register_channel({"c1", "announcements", hg::Platform::Slack});

    auto result = dir.resolve_by_name("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST(ChannelDirectoryTest, ListAll) {
    hg::ChannelDirectory dir;
    dir.register_channel({"c1", "alpha", hg::Platform::Telegram});
    dir.register_channel({"c2", "beta", hg::Platform::Discord});
    dir.register_channel({"c3", "gamma", hg::Platform::Slack});

    auto all = dir.list_all();
    EXPECT_EQ(all.size(), 3u);
}

TEST(ChannelDirectoryTest, Clear) {
    hg::ChannelDirectory dir;
    dir.register_channel({"c1", "alpha", hg::Platform::Telegram});
    dir.register_channel({"c2", "beta", hg::Platform::Discord});

    EXPECT_EQ(dir.list_all().size(), 2u);
    dir.clear();
    EXPECT_EQ(dir.list_all().size(), 0u);
    EXPECT_FALSE(dir.resolve_by_name("alpha").has_value());
}

TEST(ChannelDirectoryTest, ReRegisterUpdatesName) {
    hg::ChannelDirectory dir;
    dir.register_channel({"c1", "old_name", hg::Platform::Telegram});
    dir.register_channel({"c1", "new_name", hg::Platform::Telegram});

    EXPECT_EQ(dir.list_all().size(), 1u);
    auto found = dir.resolve_by_name("new_name");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, "c1");
}
