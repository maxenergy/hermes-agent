#include "hermes/tools/credential_files.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using hermes::tools::clear_credential_files;
using hermes::tools::credential_path;
using hermes::tools::CredentialMount;
using hermes::tools::directory_contains_symlinks;
using hermes::tools::get_cache_directory_mounts;
using hermes::tools::get_credential_file_mounts;
using hermes::tools::get_skills_directory_mount;
using hermes::tools::hermes_credentials_dir;
using hermes::tools::hermes_env_file;
using hermes::tools::hermes_home;
using hermes::tools::iter_cache_files;
using hermes::tools::iter_skills_files;
using hermes::tools::list_credential_files;
using hermes::tools::register_credential_file;
using hermes::tools::register_credential_files;
using hermes::tools::registered_credential_count;
using hermes::tools::resolve_contained_path;
using hermes::tools::safe_skills_path;

namespace fs = std::filesystem;

namespace {

class CredentialFilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::ostringstream oss;
        oss << "hermes_cred_test_" << gen();
        tmp_ = fs::temp_directory_path() / oss.str();
        fs::create_directories(tmp_);
        setenv("HERMES_HOME", tmp_.c_str(), 1);
        clear_credential_files();
    }
    void TearDown() override {
        clear_credential_files();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
        unsetenv("HERMES_HOME");
    }

    void touch(const fs::path& p, std::string_view body = "x") {
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << body;
    }

    fs::path tmp_;
};

TEST_F(CredentialFilesTest, CredentialPathUnderHermesHome) {
    auto cred = credential_path("test_key.json");
    auto home = hermes_home().string();
    EXPECT_NE(cred.string().find(home), std::string::npos);
}

TEST_F(CredentialFilesTest, EnvFileUnderHermesHome) {
    auto env = hermes_env_file();
    EXPECT_EQ(env.parent_path(), hermes_home());
    EXPECT_EQ(env.filename(), ".env");
}

TEST_F(CredentialFilesTest, CredentialsDirUnderHermesHome) {
    auto dir = hermes_credentials_dir();
    EXPECT_EQ(dir.parent_path(), hermes_home());
    EXPECT_EQ(dir.filename(), ".credentials");
}

TEST_F(CredentialFilesTest, ListEmptyWhenDirMissing) {
    auto v = list_credential_files();
    EXPECT_TRUE(v.empty());
}

TEST_F(CredentialFilesTest, ListsRegularCredentialFiles) {
    touch(hermes_credentials_dir() / "google.json", "{}");
    touch(hermes_credentials_dir() / "nested" / "twitter.json", "{}");
    auto v = list_credential_files();
    EXPECT_EQ(v.size(), 2u);
}

TEST_F(CredentialFilesTest, ResolveRejectsAbsolute) {
    auto r = resolve_contained_path("/etc/passwd");
    EXPECT_FALSE(r.has_value());
}

TEST_F(CredentialFilesTest, ResolveRejectsTraversal) {
    auto r = resolve_contained_path("../../etc/passwd");
    EXPECT_FALSE(r.has_value());
}

TEST_F(CredentialFilesTest, ResolveRejectsEmpty) {
    auto r = resolve_contained_path("");
    EXPECT_FALSE(r.has_value());
}

TEST_F(CredentialFilesTest, ResolveSucceedsForExistingFile) {
    touch(tmp_ / "ok.json", "{}");
    auto r = resolve_contained_path("ok.json");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(fs::is_regular_file(*r));
}

TEST_F(CredentialFilesTest, ResolveReturnsNulloptForMissing) {
    auto r = resolve_contained_path("nope.json");
    EXPECT_FALSE(r.has_value());
}

TEST_F(CredentialFilesTest, RegisterCredentialFileSuccess) {
    touch(tmp_ / "secret.json", "{}");
    EXPECT_TRUE(register_credential_file("secret.json"));
    EXPECT_EQ(registered_credential_count(), 1u);
}

TEST_F(CredentialFilesTest, RegisterCredentialFileRejectsAbsolute) {
    EXPECT_FALSE(register_credential_file("/etc/passwd"));
    EXPECT_EQ(registered_credential_count(), 0u);
}

TEST_F(CredentialFilesTest, RegisterCredentialFileRejectsTraversal) {
    EXPECT_FALSE(register_credential_file("../etc/passwd"));
    EXPECT_EQ(registered_credential_count(), 0u);
}

TEST_F(CredentialFilesTest, RegisterCredentialFileMissing) {
    EXPECT_FALSE(register_credential_file("missing.json"));
}

TEST_F(CredentialFilesTest, BulkRegisterMixedEntries) {
    touch(tmp_ / "a.json");
    touch(tmp_ / "b.json");
    nlohmann::json entries = nlohmann::json::array();
    entries.push_back("a.json");
    entries.push_back({{"path", "b.json"}});
    entries.push_back({{"name", "missing.json"}});
    entries.push_back("/etc/passwd");
    auto missing = register_credential_files(entries);
    EXPECT_EQ(missing.size(), 2u);
    EXPECT_EQ(registered_credential_count(), 2u);
}

TEST_F(CredentialFilesTest, BulkRegisterTrimsWhitespace) {
    touch(tmp_ / "z.json");
    nlohmann::json entries = nlohmann::json::array();
    entries.push_back("  z.json  ");
    auto missing = register_credential_files(entries);
    EXPECT_TRUE(missing.empty());
}

TEST_F(CredentialFilesTest, BulkRegisterIgnoresUnknownTypes) {
    nlohmann::json entries = nlohmann::json::array();
    entries.push_back(42);
    entries.push_back(nullptr);
    auto missing = register_credential_files(entries);
    EXPECT_TRUE(missing.empty());
}

TEST_F(CredentialFilesTest, GetCredentialFileMountsContainerPath) {
    touch(tmp_ / "tok.json");
    register_credential_file("tok.json", "/srv/.hermes");
    auto m = get_credential_file_mounts();
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].container_path, "/srv/.hermes/tok.json");
    EXPECT_NE(m[0].host_path.find("tok.json"), std::string::npos);
}

TEST_F(CredentialFilesTest, GetCredentialFileMountsSkipsDeleted) {
    touch(tmp_ / "tok.json");
    register_credential_file("tok.json");
    fs::remove(tmp_ / "tok.json");
    auto m = get_credential_file_mounts();
    EXPECT_TRUE(m.empty());
}

TEST_F(CredentialFilesTest, ClearResetsRegistry) {
    touch(tmp_ / "x.json");
    register_credential_file("x.json");
    EXPECT_EQ(registered_credential_count(), 1u);
    clear_credential_files();
    EXPECT_EQ(registered_credential_count(), 0u);
}

TEST_F(CredentialFilesTest, SkillsMountsAbsentWhenDirMissing) {
    auto v = get_skills_directory_mount();
    EXPECT_TRUE(v.empty());
}

TEST_F(CredentialFilesTest, SkillsMountsPresentWhenDirExists) {
    fs::create_directories(tmp_ / "skills" / "hello");
    touch(tmp_ / "skills" / "hello" / "SKILL.md", "demo");
    auto v = get_skills_directory_mount("/agent");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].container_path, "/agent/skills");
}

TEST_F(CredentialFilesTest, SkillsMountTrimsTrailingSlash) {
    fs::create_directories(tmp_ / "skills");
    touch(tmp_ / "skills" / "x.md");
    auto v = get_skills_directory_mount("/agent/");
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].container_path, "/agent/skills");
}

TEST_F(CredentialFilesTest, IterSkillsFilesEnumeratesRegularFiles) {
    fs::create_directories(tmp_ / "skills" / "a");
    touch(tmp_ / "skills" / "a" / "SKILL.md", "y");
    touch(tmp_ / "skills" / "a" / "scripts" / "run.sh", "y");
    auto v = iter_skills_files("/c");
    EXPECT_EQ(v.size(), 2u);
    bool saw_run = false;
    for (const auto& m : v) {
        if (m.container_path == "/c/skills/a/scripts/run.sh") saw_run = true;
    }
    EXPECT_TRUE(saw_run);
}

TEST_F(CredentialFilesTest, DirectoryContainsSymlinksDetectsTrue) {
    fs::create_directories(tmp_ / "skills");
    auto target = tmp_ / "real";
    touch(target);
    std::error_code ec;
    fs::create_symlink(target, tmp_ / "skills" / "lnk", ec);
    if (!ec) {
        EXPECT_TRUE(directory_contains_symlinks(tmp_ / "skills"));
    }
}

TEST_F(CredentialFilesTest, DirectoryContainsSymlinksFalseWhenMissing) {
    EXPECT_FALSE(directory_contains_symlinks(tmp_ / "no_such_dir"));
}

TEST_F(CredentialFilesTest, SafeSkillsPathReturnsSelfWhenNoSymlinks) {
    fs::create_directories(tmp_ / "skills" / "x");
    touch(tmp_ / "skills" / "x" / "y.md");
    auto p = safe_skills_path(tmp_ / "skills");
    EXPECT_EQ(p, tmp_ / "skills");
}

TEST_F(CredentialFilesTest, CacheMountsForDocumentsExist) {
    fs::create_directories(tmp_ / "cache" / "documents");
    touch(tmp_ / "cache" / "documents" / "f.txt");
    auto v = get_cache_directory_mounts("/cache");
    ASSERT_FALSE(v.empty());
    bool found_docs = false;
    for (const auto& m : v) {
        if (m.container_path == "/cache/cache/documents") found_docs = true;
    }
    EXPECT_TRUE(found_docs);
}

TEST_F(CredentialFilesTest, CacheMountsHonorLegacyName) {
    fs::create_directories(tmp_ / "document_cache");
    touch(tmp_ / "document_cache" / "f.txt");
    auto v = get_cache_directory_mounts();
    ASSERT_FALSE(v.empty());
    EXPECT_EQ(v[0].container_path, "/root/.hermes/cache/documents");
    EXPECT_NE(v[0].host_path.find("document_cache"), std::string::npos);
}

TEST_F(CredentialFilesTest, IterCacheFilesEnumerates) {
    fs::create_directories(tmp_ / "cache" / "images" / "sub");
    touch(tmp_ / "cache" / "images" / "sub" / "a.png");
    touch(tmp_ / "cache" / "images" / "b.png");
    auto v = iter_cache_files("/c");
    int hits = 0;
    for (const auto& m : v) {
        if (m.container_path.find("/c/cache/images/") == 0) hits++;
    }
    EXPECT_EQ(hits, 2);
}

TEST_F(CredentialFilesTest, IterCacheFilesEmptyWhenNothing) {
    auto v = iter_cache_files();
    EXPECT_TRUE(v.empty());
}

}  // namespace
