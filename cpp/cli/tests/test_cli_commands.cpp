#include "hermes/cli/hermes_cli.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace hermes::cli;

namespace {
template <typename Fn>
std::string capture_stdout(Fn fn) {
    std::ostringstream oss;
    auto* old = std::cout.rdbuf(oss.rdbuf());
    fn();
    std::cout.rdbuf(old);
    return oss.str();
}
}  // namespace

TEST(CLICommands, PersonalitySetsValue) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/personality snarky");
    });
    EXPECT_NE(out.find("Personality set to: snarky"), std::string::npos);
}

TEST(CLICommands, PersonalityShowsDefault) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/personality");
    });
    EXPECT_NE(out.find("Personality:"), std::string::npos);
}

TEST(CLICommands, FastToggles) {
    HermesCLI cli;
    auto out1 = capture_stdout([&] {
        cli.process_command("/fast");
    });
    EXPECT_NE(out1.find("Temperature set to:"), std::string::npos);
    EXPECT_NE(out1.find("0"), std::string::npos);  // First toggle: 1.0 -> 0.0

    auto out2 = capture_stdout([&] {
        cli.process_command("/fast");
    });
    EXPECT_NE(out2.find("Temperature set to:"), std::string::npos);
    EXPECT_NE(out2.find("1"), std::string::npos);  // Second toggle: 0.0 -> 1.0
}

TEST(CLICommands, YoloEnables) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/yolo");
    });
    EXPECT_NE(out.find("Auto-approve (yolo) mode: on"), std::string::npos);
}

TEST(CLICommands, YoloTogglesTwice) {
    HermesCLI cli;
    capture_stdout([&] { cli.process_command("/yolo"); });
    auto out2 = capture_stdout([&] {
        cli.process_command("/yolo");
    });
    EXPECT_NE(out2.find("Auto-approve (yolo) mode: off"), std::string::npos);
}

TEST(CLICommands, VerboseToggles) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/verbose");
    });
    EXPECT_NE(out.find("Verbose mode: on"), std::string::npos);

    auto out2 = capture_stdout([&] {
        cli.process_command("/verbose");
    });
    EXPECT_NE(out2.find("Verbose mode: off"), std::string::npos);
}

TEST(CLICommands, SkillsLists) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/skills");
    });
    EXPECT_NE(out.find("skill"), std::string::npos);
}

TEST(CLICommands, ToolsLists) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/tools");
    });
    EXPECT_NE(out.find("toolset"), std::string::npos);
}

TEST(CLICommands, VoiceSetsValue) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/voice alloy");
    });
    EXPECT_NE(out.find("Voice set to: alloy"), std::string::npos);
}

TEST(CLICommands, ReasoningSetsLevel) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/reasoning 2");
    });
    EXPECT_NE(out.find("Reasoning effort set to: 2"), std::string::npos);
}

TEST(CLICommands, InsightsShowsStats) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/insights");
    });
    EXPECT_NE(out.find("Session insights"), std::string::npos);
}

TEST(CLICommands, PlatformsShowsCLI) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/platforms");
    });
    EXPECT_NE(out.find("CLI only"), std::string::npos);
}

TEST(CLICommands, CompressDoesNotCrash) {
    HermesCLI cli;
    EXPECT_NO_THROW({
        std::ostringstream oss;
        auto* old = std::cout.rdbuf(oss.rdbuf());
        cli.process_command("/compress");
        std::cout.rdbuf(old);
    });
}

// ---------------------------------------------------------------------
// Newly-ported handlers (Stream L1).
// ---------------------------------------------------------------------

TEST(CLICommands, HistoryEmptyShowsFriendlyMessage) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/history");
    });
    EXPECT_NE(out.find("No conversation history"), std::string::npos);
}

TEST(CLICommands, SaveEmptyRefusesToWrite) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/save");
    });
    EXPECT_NE(out.find("No conversation to save"), std::string::npos);
}

TEST(CLICommands, ConfigPrintsJSONObject) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/config");
    });
    EXPECT_NE(out.find("Current configuration"), std::string::npos);
    // /config always dumps a JSON object (or "{}" at minimum).
    EXPECT_TRUE(out.find("{") != std::string::npos ||
                out.find("null") != std::string::npos);
}

TEST(CLICommands, StatusbarTogglesBoth) {
    HermesCLI cli;
    auto out1 = capture_stdout([&] { cli.process_command("/statusbar on"); });
    EXPECT_NE(out1.find("Status bar: on"), std::string::npos);
    auto out2 = capture_stdout([&] { cli.process_command("/statusbar off"); });
    EXPECT_NE(out2.find("Status bar: off"), std::string::npos);
    // Toggle without args should flip based on last state.
    auto out3 = capture_stdout([&] { cli.process_command("/statusbar"); });
    EXPECT_NE(out3.find("Status bar: on"), std::string::npos);
}

TEST(CLICommands, SkinShowsAndSets) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/skin"); });
    EXPECT_NE(out.find("Active skin"), std::string::npos);
    auto out2 = capture_stdout([&] { cli.process_command("/skin mono"); });
    EXPECT_NE(out2.find("Skin set to: mono"), std::string::npos);
}

TEST(CLICommands, ToolsetsListsEntries) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/toolsets"); });
    EXPECT_NE(out.find("Available toolsets"), std::string::npos);
}

TEST(CLICommands, BrowserStubIsGraceful) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/browser"); });
    // Status stub mentions "disconnected"; connect path prints the
    // migration note.  Either is acceptable.
    EXPECT_TRUE(out.find("Browser bridge") != std::string::npos ||
                out.find("Browser") != std::string::npos);
    auto out2 = capture_stdout([&] {
        cli.process_command("/browser connect");
    });
    EXPECT_NE(out2.find("Browser connect is not available"), std::string::npos);
}

TEST(CLICommands, PluginsHandlesMissingDir) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/plugins"); });
    // Either "No plugins installed" (empty / missing dir) or
    // "Installed plugins" (some present).  Both are valid responses.
    EXPECT_TRUE(out.find("plugins") != std::string::npos ||
                out.find("Plugins") != std::string::npos);
}

TEST(CLICommands, PasteRefusesWhenNoImage) {
    // Under CI the clipboard is empty — we just assert the handler
    // doesn't crash and prints a friendly message.
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/paste"); });
    // Either the "no image" path or a success path if the test runner
    // happens to have a PNG on the clipboard — both are fine.
    EXPECT_TRUE(out.find("No image on the clipboard") != std::string::npos ||
                out.find("Image captured") != std::string::npos);
}

TEST(CLICommands, ImageRequiresPath) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/image"); });
    EXPECT_NE(out.find("Usage: /image"), std::string::npos);
}

TEST(CLICommands, ImageRejectsMissingFile) {
    HermesCLI cli;
    auto out = capture_stdout([&] {
        cli.process_command("/image /tmp/definitely_not_a_real_file_qwx.png");
    });
    EXPECT_NE(out.find("File not found"), std::string::npos);
}

TEST(CLICommands, ClearResetsAndClears) {
    HermesCLI cli;
    auto out = capture_stdout([&] { cli.process_command("/clear"); });
    // Must emit ANSI clear screen sequence and the "New session started"
    // banner from handle_new().
    EXPECT_NE(out.find("\033[2J"), std::string::npos);
    EXPECT_NE(out.find("New session started"), std::string::npos);
}

TEST(CLICommands, CronSubcommandListRunsWithoutCrash) {
    HermesCLI cli;
    EXPECT_NO_THROW({
        auto out = capture_stdout([&] { cli.process_command("/cron list"); });
        // `cmd_list` prints via its own std::cout; at minimum the command
        // must not crash even when the JobStore file is absent.
        (void)out;
    });
}
