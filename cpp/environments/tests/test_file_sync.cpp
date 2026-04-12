#include "hermes/environments/file_sync.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace he = hermes::environments;
namespace fs = std::filesystem;

class FileSyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "hermes_file_sync_test";
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    fs::path create_file(const std::string& name,
                         const std::string& content) {
        auto path = tmp_dir_ / name;
        std::ofstream ofs(path);
        ofs << content;
        return path;
    }

    fs::path tmp_dir_;
};

TEST_F(FileSyncTest, MtimeCacheSkip) {
    auto local = create_file("test.txt", "hello");
    he::FileSyncManager mgr;

    he::FileSyncManager::FileEntry entry{local, "/remote/test.txt"};

    // First sync — file should need syncing.
    auto to_sync = mgr.files_to_sync({entry});
    EXPECT_EQ(to_sync.size(), 1u);

    // Mark as synced.
    mgr.mark_synced(entry);

    // Second check — should skip (unchanged).
    to_sync = mgr.files_to_sync({entry});
    EXPECT_EQ(to_sync.size(), 0u);
}

TEST_F(FileSyncTest, ChangedFileResyncs) {
    auto local = create_file("test.txt", "hello");
    he::FileSyncManager mgr;

    he::FileSyncManager::FileEntry entry{local, "/remote/test.txt"};
    mgr.mark_synced(entry);

    // Modify the file (change size).
    {
        std::ofstream ofs(local);
        ofs << "hello world, this is different content";
    }

    auto to_sync = mgr.files_to_sync({entry});
    EXPECT_EQ(to_sync.size(), 1u);
}

TEST_F(FileSyncTest, QuotedRmRejectsDotDot) {
    EXPECT_EQ(he::FileSyncManager::quoted_rm_command("/safe/path"),
              "rm -f '/safe/path'");
    EXPECT_EQ(he::FileSyncManager::quoted_rm_command("../escape"),
              "");
    EXPECT_EQ(he::FileSyncManager::quoted_rm_command("/a/../b"),
              "");
    EXPECT_EQ(he::FileSyncManager::quoted_rm_command(""), "");
}

TEST_F(FileSyncTest, SyncToRemoteCallsCopyFn) {
    auto local1 = create_file("a.txt", "aaa");
    auto local2 = create_file("b.txt", "bbb");

    he::FileSyncManager mgr;

    std::vector<he::FileSyncManager::FileEntry> entries = {
        {local1, "/remote/a.txt"},
        {local2, "/remote/b.txt"},
    };

    int copy_count = 0;
    auto transferred = mgr.sync_to_remote(entries,
        [&copy_count](const fs::path&, const fs::path&) {
            ++copy_count;
            return true;
        });

    EXPECT_EQ(transferred, 2u);
    EXPECT_EQ(copy_count, 2);

    // Second sync — should be cached, zero transfers.
    copy_count = 0;
    transferred = mgr.sync_to_remote(entries,
        [&copy_count](const fs::path&, const fs::path&) {
            ++copy_count;
            return true;
        });

    EXPECT_EQ(transferred, 0u);
    EXPECT_EQ(copy_count, 0);
}
