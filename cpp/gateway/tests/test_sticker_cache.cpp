#include <gtest/gtest.h>

#include <hermes/gateway/sticker_cache.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class StickerCacheTest : public ::testing::Test {
protected:
    fs::path dir_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("hermes_sticker_" +
                std::to_string(std::chrono::system_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
};

TEST_F(StickerCacheTest, StoreAndGetRoundTrip) {
    hg::StickerCache cache(dir_);
    cache.store("sticker1", "binary_data_here");

    auto got = cache.get("sticker1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "binary_data_here");
}

TEST_F(StickerCacheTest, MissingReturnsNullopt) {
    hg::StickerCache cache(dir_);
    auto got = cache.get("nonexistent");
    EXPECT_FALSE(got.has_value());
}

TEST_F(StickerCacheTest, CleanupOlderThanRemovesOldFiles) {
    hg::StickerCache cache(dir_);
    cache.store("old_sticker", "old_data");
    cache.store("new_sticker", "new_data");

    // Backdate old_sticker by 48 hours.
    auto old_path = dir_ / "old_sticker";
    auto old_time =
        fs::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code ec;
    fs::last_write_time(old_path, old_time, ec);
    ASSERT_FALSE(ec) << "failed to set mtime: " << ec.message();

    cache.cleanup_older_than(std::chrono::hours(24));

    EXPECT_FALSE(cache.get("old_sticker").has_value());
    EXPECT_TRUE(cache.get("new_sticker").has_value());
}

TEST_F(StickerCacheTest, CorruptCacheDirHandled) {
    // Non-existent subdirectory should be created by constructor.
    auto subdir = dir_ / "deep" / "nested" / "cache";
    hg::StickerCache cache(subdir);

    // Getting from empty cache returns nullopt.
    EXPECT_FALSE(cache.get("anything").has_value());

    // Cleanup on empty cache should not throw.
    EXPECT_NO_THROW(cache.cleanup_older_than(std::chrono::hours(1)));

    // Store then retrieve should still work.
    cache.store("s1", "data");
    auto got = cache.get("s1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "data");
}

TEST_F(StickerCacheTest, OverwriteExistingSticker) {
    hg::StickerCache cache(dir_);
    cache.store("s1", "first");
    cache.store("s1", "second");

    auto got = cache.get("s1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "second");
}
