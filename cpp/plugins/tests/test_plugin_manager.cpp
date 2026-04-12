#include "hermes/plugins/plugin_manager.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace hp = hermes::plugins;

class PluginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a unique temporary directory for each test.
        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("hermes_plugin_test_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::filesystem::path tmp_dir_;
};

// 1. discover() on empty directory finds nothing.
TEST_F(PluginManagerTest, DiscoverEmptyDir) {
    hp::PluginManager mgr(tmp_dir_);
    mgr.discover();
    EXPECT_TRUE(mgr.list().empty());
}

// 2. list() on freshly constructed manager returns empty vector.
TEST_F(PluginManagerTest, ListOnEmpty) {
    hp::PluginManager mgr(tmp_dir_);
    auto plugins = mgr.list();
    EXPECT_TRUE(plugins.empty());
}

// 3. load() with nonexistent plugin returns false.
TEST_F(PluginManagerTest, LoadNonexistent) {
    hp::PluginManager mgr(tmp_dir_);
    EXPECT_FALSE(mgr.load("does_not_exist"));
}

// 4. is_loaded() returns false for unknown plugin.
TEST_F(PluginManagerTest, IsLoadedUnknown) {
    hp::PluginManager mgr(tmp_dir_);
    EXPECT_FALSE(mgr.is_loaded("nope"));
}

// 5. unload() on unknown plugin returns false.
TEST_F(PluginManagerTest, UnloadUnknown) {
    hp::PluginManager mgr(tmp_dir_);
    EXPECT_FALSE(mgr.unload("nope"));
}

// 6. discover() on non-existent directory does not crash.
TEST_F(PluginManagerTest, DiscoverNonexistentDir) {
    hp::PluginManager mgr(tmp_dir_ / "nonexistent_subdir");
    EXPECT_NO_THROW(mgr.discover());
    EXPECT_TRUE(mgr.list().empty());
}

// 7. load() with a file that is not a valid shared library returns false.
TEST_F(PluginManagerTest, LoadInvalidLibrary) {
    // Create a dummy file with the right extension but invalid content.
#ifdef _WIN32
    const std::string ext = ".dll";
#elif defined(__APPLE__)
    const std::string ext = ".dylib";
#else
    const std::string ext = ".so";
#endif
    const auto path = tmp_dir_ / ("fake_plugin" + ext);
    {
        std::ofstream ofs(path);
        ofs << "not a real shared library";
    }
    hp::PluginManager mgr(tmp_dir_);
    EXPECT_FALSE(mgr.load("fake_plugin"));
    EXPECT_FALSE(mgr.is_loaded("fake_plugin"));
}

// 8. discover() skips files with wrong extension.
TEST_F(PluginManagerTest, DiscoverSkipsWrongExtension) {
    // Create a .txt file — should be ignored.
    {
        std::ofstream ofs(tmp_dir_ / "readme.txt");
        ofs << "hello";
    }
    hp::PluginManager mgr(tmp_dir_);
    mgr.discover();
    EXPECT_TRUE(mgr.list().empty());
}
