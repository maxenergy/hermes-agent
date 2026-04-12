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
