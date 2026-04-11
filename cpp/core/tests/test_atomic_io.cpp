#include "hermes/core/atomic_io.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace aio = hermes::core::atomic_io;
namespace fs = std::filesystem;

namespace {

fs::path unique_tmp(const std::string& tag) {
    auto dir = fs::temp_directory_path() / ("hermes-aio-" + tag);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST(AtomicIo, RoundTripHappyPath) {
    const auto dir = unique_tmp("round-trip");
    const auto file = dir / "data.txt";
    fs::remove(file);

    EXPECT_TRUE(aio::atomic_write(file, "hello world"));
    const auto read_back = aio::atomic_read(file);
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(*read_back, "hello world");

    fs::remove_all(dir);
}

TEST(AtomicIo, OverwriteReplacesExistingFile) {
    const auto dir = unique_tmp("overwrite");
    const auto file = dir / "data.txt";

    EXPECT_TRUE(aio::atomic_write(file, "first"));
    EXPECT_TRUE(aio::atomic_write(file, "second"));
    const auto read_back = aio::atomic_read(file);
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(*read_back, "second");

    fs::remove_all(dir);
}

TEST(AtomicIo, ReadMissingFileReturnsNullopt) {
    const auto dir = unique_tmp("missing");
    const auto file = dir / "no-such.txt";
    fs::remove(file);
    EXPECT_FALSE(aio::atomic_read(file).has_value());
    fs::remove_all(dir);
}

TEST(AtomicIo, WriteCreatesParentDirs) {
    const auto dir = unique_tmp("mkdir");
    const auto nested = dir / "a" / "b" / "c" / "data.bin";
    fs::remove_all(dir);

    EXPECT_TRUE(aio::atomic_write(nested, "\x01\x02\x03"));
    EXPECT_TRUE(fs::exists(nested));
    const auto read_back = aio::atomic_read(nested);
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(read_back->size(), 3U);

    fs::remove_all(dir);
}
