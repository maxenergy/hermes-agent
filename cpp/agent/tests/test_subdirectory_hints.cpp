#include "hermes/agent/subdirectory_hints.hpp"

#include <gtest/gtest.h>

using hermes::agent::SubdirectoryHintTracker;
namespace fs = std::filesystem;

TEST(SubdirectoryHintTracker, RecordsAndReturnsLruOrder) {
    SubdirectoryHintTracker t(8);
    t.record_edit(fs::path("/proj/src/foo.cpp"));
    t.record_edit(fs::path("/proj/include/bar.hpp"));
    t.record_edit(fs::path("/proj/tests/baz.cpp"));

    auto recent = t.recent(2);
    ASSERT_EQ(recent.size(), 2u);
    // Most-recent first.
    EXPECT_EQ(recent[0], "/proj/tests");
    EXPECT_EQ(recent[1], "/proj/include");
}

TEST(SubdirectoryHintTracker, RepeatedEditMovesToFront) {
    SubdirectoryHintTracker t(8);
    t.record_edit(fs::path("/a/x.cpp"));
    t.record_edit(fs::path("/b/y.cpp"));
    t.record_edit(fs::path("/c/z.cpp"));
    t.record_edit(fs::path("/a/x2.cpp"));  // touches /a again

    auto recent = t.recent(3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0], "/a");
    EXPECT_EQ(recent[1], "/c");
    EXPECT_EQ(recent[2], "/b");
}

TEST(SubdirectoryHintTracker, CapacityEvictsOldest) {
    SubdirectoryHintTracker t(2);
    t.record_edit(fs::path("/d1/x"));
    t.record_edit(fs::path("/d2/x"));
    t.record_edit(fs::path("/d3/x"));
    EXPECT_EQ(t.size(), 2u);
    auto r = t.recent(5);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0], "/d3");
    EXPECT_EQ(r[1], "/d2");
}

TEST(SubdirectoryHintTracker, ClearEmptiesEverything) {
    SubdirectoryHintTracker t;
    t.record_edit(fs::path("/foo/bar.txt"));
    t.clear();
    EXPECT_EQ(t.size(), 0u);
    EXPECT_TRUE(t.recent(5).empty());
}
