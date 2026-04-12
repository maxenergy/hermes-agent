#include "hermes/environments/snapshot_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
namespace he = hermes::environments;

namespace {

fs::path unique_tmp(const std::string& leaf) {
    auto dir = fs::temp_directory_path() /
               ("hermes_snapstore_" + std::to_string(::getpid()) + "_" + leaf);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "snapshots.json";
}

}  // namespace

TEST(SnapshotStore, SaveAndLoadRoundTrip) {
    auto file = unique_tmp("round_trip");
    he::SnapshotStore store(file);

    nlohmann::json snap = {{"sandbox_id", "sb-123"}, {"provider", "modal"}};
    store.save("task-a", snap);

    auto loaded = store.load("task-a");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["sandbox_id"], "sb-123");
    EXPECT_EQ((*loaded)["provider"], "modal");

    // And the file actually exists on disk.
    EXPECT_TRUE(fs::exists(file));
}

TEST(SnapshotStore, MultiTaskIsolation) {
    auto file = unique_tmp("multi_task");
    he::SnapshotStore store(file);

    store.save("task-a", {{"sandbox_id", "A"}});
    store.save("task-b", {{"sandbox_id", "B"}});

    auto a = store.load("task-a");
    auto b = store.load("task-b");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ((*a)["sandbox_id"], "A");
    EXPECT_EQ((*b)["sandbox_id"], "B");
    EXPECT_FALSE(store.load("task-c").has_value());
}

TEST(SnapshotStore, Remove) {
    auto file = unique_tmp("remove");
    he::SnapshotStore store(file);

    store.save("task-a", {{"sandbox_id", "X"}});
    ASSERT_TRUE(store.load("task-a").has_value());
    store.remove("task-a");
    EXPECT_FALSE(store.load("task-a").has_value());

    // Remove of a missing key is a no-op.
    store.remove("task-a");
}

TEST(SnapshotStore, CorruptFileHandledGracefully) {
    auto file = unique_tmp("corrupt");
    {
        std::ofstream os(file);
        os << "this is not json {{{{{";
    }
    he::SnapshotStore store(file);

    // load() on a corrupt file returns nullopt rather than throwing.
    EXPECT_FALSE(store.load("anything").has_value());

    // save() overwrites the corrupt contents.
    store.save("task-a", {{"sandbox_id", "recovered"}});
    auto loaded = store.load("task-a");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["sandbox_id"], "recovered");
}

TEST(SnapshotStore, MissingFileReturnsNullopt) {
    auto dir = fs::temp_directory_path() /
               ("hermes_snapstore_missing_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    // Intentionally do not create the file.
    he::SnapshotStore store(dir / "nope.json");
    EXPECT_FALSE(store.load("any").has_value());
}
