#include <gtest/gtest.h>

#include <hermes/gateway/message_mirror.hpp>

namespace hg = hermes::gateway;

TEST(MessageMirrorTest, AddRule) {
    hg::MessageMirror mirror;
    mirror.add_rule({hg::Platform::Telegram, "tg_chat",
                     hg::Platform::Discord, "disc_chat"});

    auto mirrors = mirror.get_mirrors(hg::Platform::Telegram, "tg_chat");
    ASSERT_EQ(mirrors.size(), 1u);
    EXPECT_EQ(mirrors[0].first, hg::Platform::Discord);
    EXPECT_EQ(mirrors[0].second, "disc_chat");
}

TEST(MessageMirrorTest, GetMirrorsForMatching) {
    hg::MessageMirror mirror;
    mirror.add_rule({hg::Platform::Telegram, "src",
                     hg::Platform::Slack, "dest1"});
    mirror.add_rule({hg::Platform::Telegram, "src",
                     hg::Platform::Discord, "dest2"});
    mirror.add_rule({hg::Platform::Telegram, "other",
                     hg::Platform::Slack, "dest3"});

    auto mirrors = mirror.get_mirrors(hg::Platform::Telegram, "src");
    EXPECT_EQ(mirrors.size(), 2u);
}

TEST(MessageMirrorTest, NoMirrorsForNonMatching) {
    hg::MessageMirror mirror;
    mirror.add_rule({hg::Platform::Telegram, "chat1",
                     hg::Platform::Slack, "dest1"});

    auto mirrors = mirror.get_mirrors(hg::Platform::Discord, "chat1");
    EXPECT_TRUE(mirrors.empty());

    auto mirrors2 = mirror.get_mirrors(hg::Platform::Telegram, "other_chat");
    EXPECT_TRUE(mirrors2.empty());
}

TEST(MessageMirrorTest, EmptyByDefault) {
    hg::MessageMirror mirror;
    auto mirrors = mirror.get_mirrors(hg::Platform::Telegram, "any");
    EXPECT_TRUE(mirrors.empty());
}
