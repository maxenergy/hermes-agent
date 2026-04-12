#include "hermes/cli/hermes_cli.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli;

TEST(HermesCLI, ProcessCommandHelp) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/help"));
}

TEST(HermesCLI, ProcessCommandExit) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/exit"));
}

TEST(HermesCLI, ProcessCommandUnknown) {
    HermesCLI cli;
    EXPECT_FALSE(cli.process_command("/xyzzy_nonexistent_command"));
}

TEST(HermesCLI, ShowBannerDoesNotCrash) {
    HermesCLI cli;
    EXPECT_NO_THROW(cli.show_banner());
}

TEST(HermesCLI, ProcessCommandNew) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/new"));
}

TEST(HermesCLI, ProcessCommandModel) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/model"));
    EXPECT_TRUE(cli.process_command("/model gpt-4o"));
}

TEST(HermesCLI, ProcessCommandUsage) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/usage"));
}

TEST(HermesCLI, ProcessCommandStatus) {
    HermesCLI cli;
    EXPECT_TRUE(cli.process_command("/status"));
}
