// Tests for MemoryStore lazy-loading cache.
#include "hermes/state/memory_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

namespace hs = hermes::state;

class MemoryCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "hermes_mem_cache_test";
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
};

TEST_F(MemoryCacheTest, SecondReadUsesCache) {
    hs::MemoryStore store(dir_);

    store.add(hs::MemoryFile::Agent, "entry1");
    store.add(hs::MemoryFile::Agent, "entry2");

    // First read populates the cache.
    auto first = store.read_all(hs::MemoryFile::Agent);
    ASSERT_EQ(first.size(), 2u);

    // Record the file mtime.
    auto path = store.path_for(hs::MemoryFile::Agent);
    auto mtime1 = std::filesystem::last_write_time(path);

    // Small delay to ensure mtime would change on a real disk read.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second read should use cache — file is not re-read.
    auto second = store.read_all(hs::MemoryFile::Agent);
    EXPECT_EQ(second.size(), 2u);

    // Verify the file has not been modified (mtime unchanged).
    auto mtime2 = std::filesystem::last_write_time(path);
    EXPECT_EQ(mtime1, mtime2);
}

TEST_F(MemoryCacheTest, CacheInvalidatedOnAdd) {
    hs::MemoryStore store(dir_);
    store.add(hs::MemoryFile::Agent, "a");

    auto r1 = store.read_all(hs::MemoryFile::Agent);
    ASSERT_EQ(r1.size(), 1u);

    store.add(hs::MemoryFile::Agent, "b");

    auto r2 = store.read_all(hs::MemoryFile::Agent);
    EXPECT_EQ(r2.size(), 2u);
}

TEST_F(MemoryCacheTest, CacheInvalidatedOnRemove) {
    hs::MemoryStore store(dir_);
    store.add(hs::MemoryFile::User, "foo");
    store.add(hs::MemoryFile::User, "bar");

    auto r1 = store.read_all(hs::MemoryFile::User);
    ASSERT_EQ(r1.size(), 2u);

    store.remove(hs::MemoryFile::User, "foo");

    auto r2 = store.read_all(hs::MemoryFile::User);
    EXPECT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0], "bar");
}

TEST_F(MemoryCacheTest, ExplicitInvalidation) {
    hs::MemoryStore store(dir_);
    store.add(hs::MemoryFile::Agent, "cached");

    auto r1 = store.read_all(hs::MemoryFile::Agent);
    ASSERT_EQ(r1.size(), 1u);

    // Externally modify the file.
    auto path = store.path_for(hs::MemoryFile::Agent);
    {
        std::ofstream f(path, std::ios::trunc);
        f << "externally modified";
    }

    // Without invalidation, cache still returns old data.
    auto r2 = store.read_all(hs::MemoryFile::Agent);
    EXPECT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0], "cached");

    // After invalidation, the new content is read.
    store.invalidate_cache();
    auto r3 = store.read_all(hs::MemoryFile::Agent);
    EXPECT_EQ(r3.size(), 1u);
    EXPECT_EQ(r3[0], "externally modified");
}
