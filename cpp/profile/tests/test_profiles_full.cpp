// Comprehensive tests for the full hermes_cli/profiles.py port.
//
// Pairs with test_profile.cpp / test_profile_bootstrap.cpp and covers
// every function added in the full port — constants, wizards, wrapper
// management, export/import, archive validation, active-profile stickiness,
// gateway cleanup semantics, and error paths.

#include "hermes/core/path.hpp"
#include "hermes/profile/profile.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace hp = hermes::profile;
namespace fs = std::filesystem;

namespace {

// Pins $HOME (and clears $HERMES_HOME) so the HOME-anchored profiles
// root lives entirely under a tmp directory.
class TempHome {
public:
    TempHome() {
        base_ = fs::temp_directory_path() /
                ("hermes-profile-full-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++counter_));
        fs::create_directories(base_);

        if (const char* h = std::getenv("HOME"); h != nullptr) {
            had_home_ = true;
            old_home_ = h;
        }
        if (const char* h = std::getenv("HERMES_HOME"); h != nullptr) {
            had_hermes_home_ = true;
            old_hermes_home_ = h;
        }
        if (const char* p = std::getenv("PATH"); p != nullptr) {
            had_path_ = true;
            old_path_ = p;
        }
        ::setenv("HOME", base_.c_str(), 1);
        ::unsetenv("HERMES_HOME");
    }
    ~TempHome() {
        std::error_code ec;
        fs::remove_all(base_, ec);
        if (had_home_) {
            ::setenv("HOME", old_home_.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
        if (had_hermes_home_) {
            ::setenv("HERMES_HOME", old_hermes_home_.c_str(), 1);
        } else {
            ::unsetenv("HERMES_HOME");
        }
        if (had_path_) ::setenv("PATH", old_path_.c_str(), 1);
    }
    const fs::path& path() const { return base_; }

private:
    fs::path base_;
    bool had_home_ = false;
    std::string old_home_;
    bool had_hermes_home_ = false;
    std::string old_hermes_home_;
    bool had_path_ = false;
    std::string old_path_;
    static inline int counter_ = 0;
};

std::string read_all(const fs::path& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
}

}  // namespace

// ---------- Constants ----------

TEST(ProfilesFull, ConstantsExposed) {
    EXPECT_EQ(hp::profile_dirs().size(), 9u);
    EXPECT_EQ(hp::clone_config_files().size(), 3u);
    EXPECT_EQ(hp::clone_subdir_files().size(), 2u);
    EXPECT_EQ(hp::clone_all_strip().size(), 3u);
}

TEST(ProfilesFull, ReservedAndSubcommandSets) {
    EXPECT_TRUE(hp::is_reserved_name("hermes"));
    EXPECT_TRUE(hp::is_reserved_name("default"));
    EXPECT_FALSE(hp::is_reserved_name("alpha"));
    EXPECT_TRUE(hp::is_hermes_subcommand("gateway"));
    EXPECT_TRUE(hp::is_hermes_subcommand("acp"));
    EXPECT_FALSE(hp::is_hermes_subcommand("coder"));
}

TEST(ProfilesFull, DefaultExportExcludedRoot) {
    EXPECT_TRUE(hp::is_default_export_excluded_root("state.db"));
    EXPECT_TRUE(hp::is_default_export_excluded_root("auth.json"));
    EXPECT_TRUE(hp::is_default_export_excluded_root(".env"));
    EXPECT_FALSE(hp::is_default_export_excluded_root("config.yaml"));
}

// ---------- Name validation ----------

TEST(ProfilesFull, ValidateName_Pattern) {
    EXPECT_TRUE(hp::is_valid_profile_name("a"));
    EXPECT_TRUE(hp::is_valid_profile_name("coder"));
    EXPECT_TRUE(hp::is_valid_profile_name("a1-b_c"));
    EXPECT_TRUE(hp::is_valid_profile_name("default"));
    EXPECT_FALSE(hp::is_valid_profile_name(""));
    EXPECT_FALSE(hp::is_valid_profile_name("-leading"));
    EXPECT_FALSE(hp::is_valid_profile_name("UPPER"));
    EXPECT_FALSE(hp::is_valid_profile_name("foo/bar"));
    EXPECT_FALSE(hp::is_valid_profile_name("foo..bar"));
    EXPECT_FALSE(hp::is_valid_profile_name(std::string(65, 'a')));
}

TEST(ProfilesFull, ValidateNameThrows) {
    EXPECT_THROW(hp::validate_profile_name("BAD"), std::invalid_argument);
    EXPECT_NO_THROW(hp::validate_profile_name("good1"));
}

// ---------- Path helpers ----------

TEST(ProfilesFull, PathsHomeAnchored) {
    TempHome home;
    EXPECT_EQ(hp::get_default_hermes_home(), home.path() / ".hermes");
    EXPECT_EQ(hp::get_profiles_root(), home.path() / ".hermes" / "profiles");
    EXPECT_EQ(hp::get_active_profile_path(),
              home.path() / ".hermes" / "active_profile");
    EXPECT_EQ(hp::get_wrapper_dir(), home.path() / ".local" / "bin");
}

// ---------- CreateOptions / full create ----------

TEST(ProfilesFull, CreateExBootstrapsProfileDirs) {
    TempHome home;
    hp::CreateOptions opts;
    const auto dir = hp::create_profile_ex("fresh", opts);
    for (const auto& sub : hp::profile_dirs()) {
        EXPECT_TRUE(fs::is_directory(dir / sub)) << sub;
    }
    EXPECT_TRUE(fs::exists(dir / "config.yaml"));
}

TEST(ProfilesFull, CreateExRejectsDefault) {
    TempHome home;
    EXPECT_THROW(hp::create_profile_ex("default", {}), std::runtime_error);
}

TEST(ProfilesFull, CreateExCloneConfig) {
    TempHome home;
    // Seed source profile
    hp::create_profile_ex("src", {});
    {
        std::ofstream f(hp::get_profile_dir("src") / "config.yaml");
        f << "model: abc\n";
    }
    {
        std::ofstream f(hp::get_profile_dir("src") / ".env");
        f << "TOKEN=1\n";
    }
    hp::CreateOptions opts;
    opts.clone_from = "src";
    opts.clone_config = true;
    hp::create_profile_ex("dst", opts);
    EXPECT_NE(read_all(hp::get_profile_dir("dst") / "config.yaml").find("abc"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(hp::get_profile_dir("dst") / ".env"));
}

TEST(ProfilesFull, CreateExCloneAllCopiesAndStrips) {
    TempHome home;
    hp::create_profile_ex("src", {});
    {
        std::ofstream f(hp::get_profile_dir("src") / "gateway.pid");
        f << "99999";  // will be stripped
    }
    hp::CreateOptions opts;
    opts.clone_from = "src";
    opts.clone_all = true;
    hp::create_profile_ex("copy", opts);
    EXPECT_FALSE(fs::exists(hp::get_profile_dir("copy") / "gateway.pid"));
}

TEST(ProfilesFull, CreateExMissingCloneSourceThrows) {
    TempHome home;
    hp::CreateOptions opts;
    opts.clone_from = "ghost";
    EXPECT_THROW(hp::create_profile_ex("x", opts), std::runtime_error);
}

TEST(ProfilesFull, CreateExDuplicateThrows) {
    TempHome home;
    hp::create_profile_ex("dup", {});
    EXPECT_THROW(hp::create_profile_ex("dup", {}), std::runtime_error);
}

// ---------- Delete (interactive) ----------

TEST(ProfilesFull, DeleteExRemovesAndReportsPath) {
    TempHome home;
    hp::create_profile_ex("toss", {});
    const auto dir = hp::delete_profile_ex("toss", /*yes=*/true);
    EXPECT_FALSE(fs::exists(dir));
}

TEST(ProfilesFull, DeleteExRejectsDefault) {
    TempHome home;
    EXPECT_THROW(hp::delete_profile_ex("default", true), std::runtime_error);
}

TEST(ProfilesFull, DeleteExMissingThrows) {
    TempHome home;
    EXPECT_THROW(hp::delete_profile_ex("ghost", true), std::runtime_error);
}

// ---------- Config-model parsing ----------

TEST(ProfilesFull, ReadConfigModelScalar) {
    TempHome home;
    const auto dir = hp::create_profile_ex("p1", {});
    {
        std::ofstream f(dir / "config.yaml");
        f << "model: \"claude-opus\"\n";
    }
    std::string m, prov;
    hp::read_config_model(dir, m, prov);
    EXPECT_EQ(m, "claude-opus");
    EXPECT_TRUE(prov.empty());
}

TEST(ProfilesFull, ReadConfigModelMappingDefault) {
    TempHome home;
    const auto dir = hp::create_profile_ex("p2", {});
    {
        std::ofstream f(dir / "config.yaml");
        f << "model:\n  default: gpt-4\n  provider: openai\n";
    }
    std::string m, prov;
    hp::read_config_model(dir, m, prov);
    EXPECT_EQ(m, "gpt-4");
    EXPECT_EQ(prov, "openai");
}

TEST(ProfilesFull, ReadConfigModelEmptyConfig) {
    TempHome home;
    const auto dir = hp::create_profile_ex("p3", {});
    std::string m, prov;
    hp::read_config_model(dir, m, prov);
    EXPECT_TRUE(m.empty());
    EXPECT_TRUE(prov.empty());
}

// ---------- Skills counting ----------

TEST(ProfilesFull, CountSkillsSkipsHidden) {
    TempHome home;
    const auto dir = hp::create_profile_ex("sk", {});
    fs::create_directories(dir / "skills" / "alpha");
    { std::ofstream f(dir / "skills" / "alpha" / "SKILL.md"); f << "x"; }
    fs::create_directories(dir / "skills" / ".hub");
    { std::ofstream f(dir / "skills" / ".hub" / "SKILL.md"); f << "x"; }
    fs::create_directories(dir / "skills" / ".git");
    { std::ofstream f(dir / "skills" / ".git" / "SKILL.md"); f << "x"; }
    EXPECT_EQ(hp::count_skills(dir), 1);
}

// ---------- Active profile ----------

TEST(ProfilesFull, ActiveProfileDefault) {
    TempHome home;
    EXPECT_EQ(hp::get_active_profile(), "default");
}

TEST(ProfilesFull, ActiveProfileRoundtrip) {
    TempHome home;
    hp::create_profile_ex("a1", {});
    hp::set_active_profile("a1");
    EXPECT_EQ(hp::get_active_profile(), "a1");
    hp::set_active_profile("default");
    EXPECT_EQ(hp::get_active_profile(), "default");
}

TEST(ProfilesFull, SetActiveRejectsMissing) {
    TempHome home;
    EXPECT_THROW(hp::set_active_profile("nope"), std::runtime_error);
}

TEST(ProfilesFull, ActiveProfileNameFromHermesHome) {
    TempHome home;
    hp::create_profile_ex("myname", {});
    hp::apply_profile_override(std::string("myname"));
    EXPECT_EQ(hp::get_active_profile_name(), "myname");
}

// ---------- List infos ----------

TEST(ProfilesFull, ListProfileInfosIncludesDefault) {
    TempHome home;
    fs::create_directories(home.path() / ".hermes");
    hp::create_profile_ex("alpha", {});
    hp::create_profile_ex("beta", {});
    const auto infos = hp::list_profile_infos();
    ASSERT_GE(infos.size(), 3u);  // default + 2
    bool saw_default = false, saw_alpha = false, saw_beta = false;
    for (const auto& i : infos) {
        if (i.name == "default") {
            saw_default = true;
            EXPECT_TRUE(i.is_default);
        }
        if (i.name == "alpha") saw_alpha = true;
        if (i.name == "beta") saw_beta = true;
    }
    EXPECT_TRUE(saw_default);
    EXPECT_TRUE(saw_alpha);
    EXPECT_TRUE(saw_beta);
}

// ---------- Archive path validation ----------

TEST(ProfilesFull, ArchiveRejectsAbsolutePath) {
    EXPECT_THROW(hp::normalize_profile_archive_parts("/etc/passwd"),
                 std::runtime_error);
}

TEST(ProfilesFull, ArchiveRejectsParentEscape) {
    EXPECT_THROW(hp::normalize_profile_archive_parts("foo/../bar"),
                 std::runtime_error);
}

TEST(ProfilesFull, ArchiveRejectsEmpty) {
    EXPECT_THROW(hp::normalize_profile_archive_parts(""),
                 std::runtime_error);
}

TEST(ProfilesFull, ArchiveAcceptsSafePath) {
    const auto parts = hp::normalize_profile_archive_parts("profile/sub/file");
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "profile");
    EXPECT_EQ(parts[2], "file");
}

TEST(ProfilesFull, ArchiveNormalizesBackslashes) {
    const auto parts = hp::normalize_profile_archive_parts("a\\b\\c");
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
}

// ---------- Export / Import (actual tar) ----------

TEST(ProfilesFull, ExportAndImportRoundtrip) {
    TempHome home;
    hp::create_profile_ex("exporter", {});
    {
        std::ofstream f(hp::get_profile_dir("exporter") / "config.yaml");
        f << "model: export-me\n";
    }
    {
        std::ofstream f(hp::get_profile_dir("exporter") / ".env");
        f << "SECRET=redacted\n";
    }
    const auto archive = home.path() / "out.tar.gz";
    const auto actual = hp::export_profile("exporter", archive);
    ASSERT_TRUE(fs::exists(actual)) << actual.string();

    hp::delete_profile_ex("exporter", true);

    const auto imported = hp::import_profile(actual, {});
    EXPECT_TRUE(fs::exists(imported / "config.yaml"));
    // .env must have been stripped during export of a named profile
    EXPECT_FALSE(fs::exists(imported / ".env"));
}

TEST(ProfilesFull, ImportRejectsDefault) {
    TempHome home;
    // Fake a tarball whose top dir is "default".
    fs::create_directories(home.path() / "stage" / "default");
    {
        std::ofstream f(home.path() / "stage" / "default" / "x.txt");
        f << "hi";
    }
    const auto archive = home.path() / "def.tar.gz";
    std::string cmd = "tar -czf " + archive.string() + " -C " +
                      (home.path() / "stage").string() + " default";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    EXPECT_THROW(hp::import_profile(archive, {}), std::runtime_error);
}

// ---------- Wrapper scripts ----------

TEST(ProfilesFull, WrapperCreateAndRemove) {
    TempHome home;
    const auto path = hp::create_wrapper_script("coder");
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(fs::exists(path));
    const std::string body = read_all(path);
    EXPECT_NE(body.find("hermes -p coder"), std::string::npos);
    EXPECT_TRUE(hp::remove_wrapper_script("coder"));
    EXPECT_FALSE(fs::exists(path));
}

TEST(ProfilesFull, RemoveWrapperRefusesForeign) {
    TempHome home;
    const auto wrapper_dir = hp::get_wrapper_dir();
    fs::create_directories(wrapper_dir);
    std::ofstream f(wrapper_dir / "mine");
    f << "#!/bin/sh\necho hi\n";
    f.close();
    EXPECT_FALSE(hp::remove_wrapper_script("mine"));
    EXPECT_TRUE(fs::exists(wrapper_dir / "mine"));
}

TEST(ProfilesFull, AliasCollisionReserved) {
    TempHome home;
    const auto msg = hp::check_alias_collision("hermes");
    EXPECT_NE(msg.find("reserved"), std::string::npos);
}

TEST(ProfilesFull, AliasCollisionSubcommand) {
    TempHome home;
    const auto msg = hp::check_alias_collision("gateway");
    EXPECT_NE(msg.find("subcommand"), std::string::npos);
}

TEST(ProfilesFull, AliasCollisionPathBinary) {
    TempHome home;
    // Create a fake binary dir and add it to PATH.
    fs::create_directories(home.path() / "fakebin");
    {
        std::ofstream f(home.path() / "fakebin" / "occupied");
        f << "#!/bin/sh\n";
    }
    ::setenv("PATH", (home.path() / "fakebin").c_str(), 1);
    const auto msg = hp::check_alias_collision("occupied");
    EXPECT_NE(msg.find("existing command"), std::string::npos);
}

TEST(ProfilesFull, AliasNoCollisionForNewName) {
    TempHome home;
    ::setenv("PATH", (home.path() / "emptybin").c_str(), 1);
    EXPECT_TRUE(hp::check_alias_collision("brandnew").empty());
}

TEST(ProfilesFull, WrapperDirInPathDetects) {
    TempHome home;
    const auto wrapper = hp::get_wrapper_dir().string();
    ::setenv("PATH", (wrapper + ":/usr/bin").c_str(), 1);
    EXPECT_TRUE(hp::is_wrapper_dir_in_path());
    ::setenv("PATH", "/usr/bin", 1);
    EXPECT_FALSE(hp::is_wrapper_dir_in_path());
}

// ---------- Completion scripts ----------

TEST(ProfilesFull, BashCompletionScript) {
    const auto s = hp::generate_bash_completion();
    EXPECT_NE(s.find("_hermes_completion"), std::string::npos);
    EXPECT_NE(s.find("complete -F"), std::string::npos);
}

TEST(ProfilesFull, ZshCompletionScript) {
    const auto s = hp::generate_zsh_completion();
    EXPECT_NE(s.find("#compdef hermes"), std::string::npos);
    EXPECT_NE(s.find("_arguments"), std::string::npos);
}

// ---------- Resolve profile env ----------

TEST(ProfilesFull, ResolveProfileEnvDefault) {
    TempHome home;
    EXPECT_EQ(hp::resolve_profile_env("default"),
              (home.path() / ".hermes").string());
}

TEST(ProfilesFull, ResolveProfileEnvMissingThrows) {
    TempHome home;
    EXPECT_THROW(hp::resolve_profile_env("absent"), std::runtime_error);
}

TEST(ProfilesFull, ResolveProfileEnvExisting) {
    TempHome home;
    hp::create_profile_ex("r1", {});
    EXPECT_EQ(hp::resolve_profile_env("r1"),
              (home.path() / ".hermes" / "profiles" / "r1").string());
}

// ---------- Rename updates sticky state ----------

TEST(ProfilesFull, RenameUpdatesActiveProfileSticky) {
    TempHome home;
    hp::create_profile_ex("first", {});
    hp::set_active_profile("first");
    EXPECT_EQ(hp::get_active_profile(), "first");
    hp::rename_profile("first", "second");
    EXPECT_EQ(hp::get_active_profile(), "second");
}

TEST(ProfilesFull, RenameRejectsReservedTarget) {
    TempHome home;
    hp::create_profile_ex("ok", {});
    EXPECT_THROW(hp::rename_profile("ok", "default"), std::runtime_error);
}

TEST(ProfilesFull, ProfileExists) {
    TempHome home;
    EXPECT_TRUE(hp::profile_exists("default"));
    EXPECT_FALSE(hp::profile_exists("ghost"));
    hp::create_profile_ex("live", {});
    EXPECT_TRUE(hp::profile_exists("live"));
}
