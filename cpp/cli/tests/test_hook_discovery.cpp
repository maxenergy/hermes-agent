#include <gtest/gtest.h>

#include <hermes/cli/hook_discovery.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class HookDiscoveryTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("hermes_hook_discovery_" +
                 std::to_string(std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
        hermes::cli::set_fake_executor(nullptr);
    }

    void write_file(const fs::path& p, const std::string& body) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << body;
    }
};

TEST_F(HookDiscoveryTest, ReturnsEmptyForMissingDir) {
    auto out = hermes::cli::discover_hooks_in(root_ / "does_not_exist");
    EXPECT_TRUE(out.empty());
}

TEST_F(HookDiscoveryTest, ParsesHookYamlInSubdirectory) {
    write_file(root_ / "audit" / "HOOK.yaml", R"(
name: audit
event: pre-tool
match: read_file
command: echo "{}"
)");

    auto hooks = hermes::cli::discover_hooks_in(root_);
    ASSERT_EQ(hooks.size(), 1u);
    EXPECT_EQ(hooks[0].name, "audit");
    EXPECT_EQ(hooks[0].event, hermes::cli::HookEvent::PreTool);
    EXPECT_EQ(hooks[0].match, "read_file");
    EXPECT_FALSE(hooks[0].command.empty());
}

TEST_F(HookDiscoveryTest, ParsesAllSupportedEventStrings) {
    auto write = [&](const std::string& name, const std::string& evt) {
        write_file(root_ / name / "HOOK.yaml",
                   "name: " + name + "\nevent: " + evt + "\ncommand: /bin/true\n");
    };
    write("a", "pre-tool");
    write("b", "post-tool");
    write("c", "session-start");
    write("d", "session-end");
    write("e", "user-prompt");

    auto hooks = hermes::cli::discover_hooks_in(root_);
    ASSERT_EQ(hooks.size(), 5u);

    // discovery is sorted by directory name -> a..e.
    EXPECT_EQ(hooks[0].event, hermes::cli::HookEvent::PreTool);
    EXPECT_EQ(hooks[1].event, hermes::cli::HookEvent::PostTool);
    EXPECT_EQ(hooks[2].event, hermes::cli::HookEvent::SessionStart);
    EXPECT_EQ(hooks[3].event, hermes::cli::HookEvent::SessionEnd);
    EXPECT_EQ(hooks[4].event, hermes::cli::HookEvent::UserPrompt);
}

TEST_F(HookDiscoveryTest, IgnoresInvalidManifest) {
    write_file(root_ / "broken" / "HOOK.yaml", "this is: [not: valid yaml");
    auto hooks = hermes::cli::discover_hooks_in(root_);
    EXPECT_TRUE(hooks.empty());
}

TEST_F(HookDiscoveryTest, RecognisesBareScripts) {
    write_file(root_ / "raw.sh", "#!/bin/sh\necho hi\n");
    write_file(root_ / "side.py", "print('hi')\n");
    auto hooks = hermes::cli::discover_hooks_in(root_);
    ASSERT_EQ(hooks.size(), 2u);
    // Sorted: raw.sh, side.py.
    EXPECT_EQ(hooks[0].name, "raw");
    EXPECT_EQ(hooks[0].event, hermes::cli::HookEvent::Unknown);
    EXPECT_EQ(hooks[1].name, "side");
}

TEST_F(HookDiscoveryTest, SelectMatchingFiltersByEventAndGlob) {
    write_file(root_ / "a" / "HOOK.yaml",
               "name: a\nevent: pre-tool\nmatch: read_*\ncommand: x\n");
    write_file(root_ / "b" / "HOOK.yaml",
               "name: b\nevent: pre-tool\nmatch: write_file\ncommand: x\n");
    write_file(root_ / "c" / "HOOK.yaml",
               "name: c\nevent: post-tool\ncommand: x\n");

    auto hooks = hermes::cli::discover_hooks_in(root_);
    auto matched = hermes::cli::select_matching(
        hooks, hermes::cli::HookEvent::PreTool, "read_file");
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched[0].name, "a");

    auto post = hermes::cli::select_matching(
        hooks, hermes::cli::HookEvent::PostTool, "anything");
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].name, "c");
}

TEST_F(HookDiscoveryTest, ExecuteHookViaFakeExecutorContinue) {
    hermes::cli::HookManifest m;
    m.name = "fake";
    m.event = hermes::cli::HookEvent::PreTool;
    m.command = "noop";

    std::string captured_stdin;
    std::string captured_command;
    hermes::cli::set_fake_executor(
        [&](const hermes::cli::ExecutorInput& in) {
            captured_command = in.command;
            captured_stdin = in.stdin_payload;
            return hermes::cli::ExecutorOutput{
                R"({"action":"continue","message":"ok"})", "", 0};
        });

    auto r = hermes::cli::execute_hook(
        m, nlohmann::json{{"tool", "read_file"}});
    EXPECT_EQ(r.action, hermes::cli::HookResult::Action::Continue);
    EXPECT_EQ(r.message, "ok");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(captured_command, "noop");
    EXPECT_NE(captured_stdin.find("read_file"), std::string::npos);
}

TEST_F(HookDiscoveryTest, ExecuteHookViaFakeExecutorBlock) {
    hermes::cli::HookManifest m;
    m.name = "deny";
    m.command = "noop";

    hermes::cli::set_fake_executor(
        [&](const hermes::cli::ExecutorInput&) {
            return hermes::cli::ExecutorOutput{
                R"({"action":"block","message":"nope"})", "", 0};
        });

    auto r = hermes::cli::execute_hook(m, nlohmann::json::object());
    EXPECT_EQ(r.action, hermes::cli::HookResult::Action::Block);
    EXPECT_EQ(r.message, "nope");
}

TEST_F(HookDiscoveryTest, ExecuteHookFallsBackOnInvalidJsonNonZeroExit) {
    hermes::cli::HookManifest m;
    m.name = "junk";
    m.command = "noop";

    hermes::cli::set_fake_executor(
        [&](const hermes::cli::ExecutorInput&) {
            return hermes::cli::ExecutorOutput{"not json", "stderr msg", 5};
        });

    auto r = hermes::cli::execute_hook(m, nlohmann::json::object());
    // Non-zero exit + no JSON -> block.
    EXPECT_EQ(r.action, hermes::cli::HookResult::Action::Block);
    EXPECT_EQ(r.exit_code, 5);
    EXPECT_EQ(r.raw_stderr, "stderr msg");
}

#ifndef _WIN32
TEST_F(HookDiscoveryTest, ExecuteHookViaRealSubprocess) {
    // Real spawn test -- only runs on POSIX.  Echoes the JSON we wrote
    // to stdin straight back, after wrapping in the expected envelope.
    hermes::cli::set_fake_executor(nullptr);

    hermes::cli::HookManifest m;
    m.name = "echo";
    m.command = R"(printf '{"action":"continue","message":"hi"}\n')";

    auto r = hermes::cli::execute_hook(m, nlohmann::json{{"x", 1}});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.action, hermes::cli::HookResult::Action::Continue);
    EXPECT_EQ(r.message, "hi");
}
#endif

TEST_F(HookDiscoveryTest, EventToFromString) {
    EXPECT_EQ(hermes::cli::event_to_string(hermes::cli::HookEvent::PreTool),
              "pre-tool");
    EXPECT_EQ(hermes::cli::parse_event("pre_tool"),
              hermes::cli::HookEvent::PreTool);
    EXPECT_EQ(hermes::cli::parse_event("session:start"),
              hermes::cli::HookEvent::SessionStart);
    EXPECT_EQ(hermes::cli::parse_event("nonsense"),
              hermes::cli::HookEvent::Unknown);
}

}  // namespace
