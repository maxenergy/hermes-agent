#include "hermes/core/atomic_io.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>

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

// Upstream: utils.py _preserve_file_mode / _restore_file_mode (commit
// df714add). ``::open`` creates the temp file at 0600-ish under
// Hermes's umask; without restoring the original mode, a rewrite of
// a user-owned config.yaml / .env destroys the broader bits that
// Docker volume mounts or NAS shares require.
TEST(AtomicIo, PreservesExistingFileMode) {
    const auto dir = unique_tmp("preserve-mode");
    const auto file = dir / "data.txt";
    fs::remove(file);

    // Seed the destination with 0600.
    {
        std::ofstream out(file);
        out << "initial";
    }
    ASSERT_EQ(::chmod(file.c_str(), 0600), 0);

    struct stat before{};
    ASSERT_EQ(::stat(file.c_str(), &before), 0);
    ASSERT_EQ(before.st_mode & 07777, static_cast<mode_t>(0600));

    // Atomic rewrite.
    EXPECT_TRUE(aio::atomic_write(file, "rewritten"));

    struct stat after{};
    ASSERT_EQ(::stat(file.c_str(), &after), 0);
    EXPECT_EQ(after.st_mode & 07777, static_cast<mode_t>(0600))
        << "atomic_write replaced 0600 with "
        << std::oct << (after.st_mode & 07777);

    const auto read_back = aio::atomic_read(file);
    ASSERT_TRUE(read_back.has_value());
    EXPECT_EQ(*read_back, "rewritten");

    fs::remove_all(dir);
}

TEST(AtomicIo, PreservesBroaderFileMode) {
    const auto dir = unique_tmp("preserve-mode-wide");
    const auto file = dir / "shared.yaml";
    fs::remove(file);

    {
        std::ofstream out(file);
        out << "shared: true\n";
    }
    // 0664 — the style a Docker volume mount might end up with.
    ASSERT_EQ(::chmod(file.c_str(), 0664), 0);

    EXPECT_TRUE(aio::atomic_write(file, "shared: still_true\n"));

    struct stat after{};
    ASSERT_EQ(::stat(file.c_str(), &after), 0);
    EXPECT_EQ(after.st_mode & 07777, static_cast<mode_t>(0664))
        << "atomic_write tightened 0664 to "
        << std::oct << (after.st_mode & 07777);

    fs::remove_all(dir);
}

TEST(AtomicIo, NewFileGetsDefaultMode) {
    // When the destination does not pre-exist there is no original mode
    // to preserve — the temp file's creation bits (munged by umask)
    // win.  Just assert we don't crash and the file ends up readable.
    const auto dir = unique_tmp("new-file-mode");
    const auto file = dir / "fresh.txt";
    fs::remove(file);

    EXPECT_TRUE(aio::atomic_write(file, "hello"));
    struct stat after{};
    ASSERT_EQ(::stat(file.c_str(), &after), 0);
    EXPECT_TRUE((after.st_mode & S_IRUSR) != 0);
    EXPECT_TRUE((after.st_mode & S_IWUSR) != 0);

    fs::remove_all(dir);
}
