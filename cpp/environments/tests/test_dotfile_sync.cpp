#include "hermes/environments/dotfile_sync.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace he = hermes::environments;
namespace fs = std::filesystem;

namespace {

fs::path make_tmp_home() {
    char tpl[] = "/tmp/hermes-dotfile-home-XXXXXX";
    char* d = ::mkdtemp(tpl);
    return fs::path(d ? d : "");
}

void write(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p);
    ofs << content;
}

std::string read(const fs::path& p) {
    std::ifstream ifs(p);
    std::string s((std::istreambuf_iterator<char>(ifs)),
                  std::istreambuf_iterator<char>());
    return s;
}

}  // namespace

TEST(DotfileManager, DefaultListIncludesGitAndShellRc) {
    auto files = he::DotfileManager::default_files();
    EXPECT_NE(std::find(files.begin(), files.end(), ".gitconfig"),
              files.end());
    EXPECT_NE(std::find(files.begin(), files.end(), ".bashrc"),
              files.end());
    EXPECT_NE(std::find(files.begin(), files.end(), ".ssh/config"),
              files.end());
}

TEST(DotfileManager, SanitizeSshConfigStripsIdentityFile) {
    std::string in =
        "Host example\n"
        "    HostName example.com\n"
        "    IdentityFile ~/.ssh/id_rsa\n"
        "    User alice\n";
    auto out = he::DotfileManager::sanitize_ssh_config(in);
    EXPECT_EQ(out.find("IdentityFile"), std::string::npos);
    EXPECT_NE(out.find("HostName example.com"), std::string::npos);
    EXPECT_NE(out.find("User alice"), std::string::npos);
}

TEST(DotfileManager, SanitizeSshConfigStripsControlPathCaseInsensitive) {
    std::string in = "controlpath /tmp/cm-%r@%h:%p\n"
                     "ControlMaster auto\n"
                     "ControlPersist 10m\n"
                     "CertificateFile /secret/cert\n"
                     "IdentityAgent SSH_AUTH_SOCK\n"
                     "Port 22\n";
    auto out = he::DotfileManager::sanitize_ssh_config(in);
    EXPECT_EQ(out.find("controlpath"), std::string::npos);
    EXPECT_EQ(out.find("ControlMaster"), std::string::npos);
    EXPECT_EQ(out.find("ControlPersist"), std::string::npos);
    EXPECT_EQ(out.find("CertificateFile"), std::string::npos);
    EXPECT_EQ(out.find("IdentityAgent"), std::string::npos);
    EXPECT_NE(out.find("Port 22"), std::string::npos);
}

TEST(DotfileManager, SanitizeWithEqualsSeparator) {
    std::string in = "IdentityFile=~/.ssh/id_ed25519\nUser=alice\n";
    auto out = he::DotfileManager::sanitize_ssh_config(in);
    EXPECT_EQ(out.find("IdentityFile"), std::string::npos);
    EXPECT_NE(out.find("User=alice"), std::string::npos);
}

TEST(DotfileManager, SanitizeKeepsCommentsAndIncludes) {
    std::string in = "# a comment with IdentityFile inside\n"
                     "Include ~/.ssh/conf.d/*\n";
    auto out = he::DotfileManager::sanitize_ssh_config(in);
    // Comments are unaffected (ltrim doesn't match a leading '#').
    EXPECT_NE(out.find("# a comment"), std::string::npos);
    EXPECT_NE(out.find("Include"), std::string::npos);
}

TEST(DotfileManager, StageSkipsMissingFiles) {
    auto home = make_tmp_home();
    he::DotfileManager::Config cfg;
    cfg.local_home = home;
    cfg.remote_home = "/root";
    // Only .gitconfig exists.
    write(home / ".gitconfig", "[user]\n\tname = Alice\n");
    he::DotfileManager mgr(cfg);
    auto staged = mgr.stage();
    ASSERT_EQ(staged.size(), 1u);
    EXPECT_EQ(staged[0].remote_path, fs::path("/root/.gitconfig"));
    EXPECT_FALSE(staged[0].sanitized);

    fs::remove_all(home);
}

TEST(DotfileManager, StageSanitizesSshConfig) {
    auto home = make_tmp_home();
    he::DotfileManager::Config cfg;
    cfg.local_home = home;
    write(home / ".ssh" / "config",
          "Host prod\n  IdentityFile ~/.ssh/id_rsa\n  User root\n");
    he::DotfileManager mgr(cfg);
    auto staged = mgr.stage();

    bool saw_ssh = false;
    for (const auto& s : staged) {
        if (s.remote_path.filename() == "config" &&
            s.remote_path.parent_path().filename() == ".ssh") {
            saw_ssh = true;
            EXPECT_TRUE(s.sanitized);
            // Local is a temp file, not the original.
            EXPECT_NE(s.local_path, home / ".ssh" / "config");
            auto sanitized_content = read(s.local_path);
            EXPECT_EQ(sanitized_content.find("IdentityFile"),
                      std::string::npos);
            EXPECT_NE(sanitized_content.find("User root"),
                      std::string::npos);
        }
    }
    EXPECT_TRUE(saw_ssh);

    fs::remove_all(home);
}

TEST(DotfileManager, UploadInvokesCallbackPerFile) {
    auto home = make_tmp_home();
    he::DotfileManager::Config cfg;
    cfg.local_home = home;
    cfg.remote_home = "/home/sandbox";
    write(home / ".bashrc", "alias ll='ls -la'\n");
    write(home / ".vimrc", "set number\n");

    he::DotfileManager mgr(cfg);
    std::vector<std::pair<fs::path, fs::path>> copied;
    auto n = mgr.upload([&](const fs::path& l, const fs::path& r) {
        copied.emplace_back(l, r);
        return true;
    });
    EXPECT_EQ(n, 2u);
    ASSERT_EQ(copied.size(), 2u);

    // Remote paths should be under /home/sandbox/...
    for (const auto& [_, r] : copied) {
        EXPECT_EQ(r.string().rfind("/home/sandbox/", 0), 0u);
    }

    fs::remove_all(home);
}

TEST(DotfileManager, UploadCountsOnlySuccesses) {
    auto home = make_tmp_home();
    he::DotfileManager::Config cfg;
    cfg.local_home = home;
    write(home / ".bashrc", "x\n");
    write(home / ".vimrc", "y\n");
    he::DotfileManager mgr(cfg);

    int call = 0;
    auto n = mgr.upload([&](const fs::path&, const fs::path&) {
        // Fail every other call.
        return (++call) % 2 == 1;
    });
    EXPECT_EQ(n, 1u);

    fs::remove_all(home);
}
