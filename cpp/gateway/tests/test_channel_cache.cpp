// Tests for the per-platform channel cache.
#include <gtest/gtest.h>

#include <hermes/gateway/channel_cache.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class ChannelCacheEnv : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_ = fs::temp_directory_path() /
               ("hermes-cc-" + std::to_string(::getpid()) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(tmp_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }
    fs::path tmp_;
};

}  // namespace

TEST_F(ChannelCacheEnv, RoundTripPreservesEntries) {
    hermes::gateway::ChannelCache cache("telegram", tmp_);

    hermes::gateway::ChannelEntry e;
    e.id = "1234";
    e.name = "team-alerts";
    e.kind = "group";
    e.extras["forum_topic_id"] = "42";
    cache.upsert(e);
    cache.save();

    EXPECT_TRUE(fs::exists(tmp_ / "telegram" / "channels.json"));

    hermes::gateway::ChannelCache cache2("telegram", tmp_);
    cache2.load();
    auto got = cache2.by_id("1234");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "team-alerts");
    EXPECT_EQ(got->kind, "group");
    EXPECT_EQ(got->extras.at("forum_topic_id"), "42");
}

TEST_F(ChannelCacheEnv, ByNameLookup) {
    hermes::gateway::ChannelCache cache("slack", tmp_);
    cache.upsert({"C123", "general", "channel", {}});
    auto got = cache.by_name("general");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "C123");
    EXPECT_FALSE(cache.by_name("missing").has_value());
}

TEST_F(ChannelCacheEnv, MissingFileLoadsEmpty) {
    hermes::gateway::ChannelCache cache("discord", tmp_);
    cache.load();
    EXPECT_TRUE(cache.entries().empty());
}

TEST_F(ChannelCacheEnv, CorruptFileResetsToEmpty) {
    auto dir = tmp_ / "telegram";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "channels.json");
        out << "{not json";
    }
    hermes::gateway::ChannelCache cache("telegram", tmp_);
    cache.load();
    EXPECT_TRUE(cache.entries().empty());
}
