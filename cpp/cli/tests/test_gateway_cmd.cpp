// Tests for hermes::cli::gateway_cmd.
#include "hermes/cli/gateway_cmd.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::gateway_cmd;

TEST(GatewayCmd, RenderServiceUnit_ContainsExec) {
    auto body = render_service_unit("/usr/bin/hermes");
    EXPECT_NE(body.find("[Service]"), std::string::npos);
    EXPECT_NE(body.find("ExecStart=/usr/bin/hermes gateway --run"),
              std::string::npos);
    EXPECT_NE(body.find("[Install]"), std::string::npos);
    EXPECT_NE(body.find("Restart=on-failure"), std::string::npos);
}

TEST(GatewayCmd, RenderServiceUnit_DescriptionPresent) {
    auto body = render_service_unit("hermes");
    EXPECT_NE(body.find("Description=Hermes messaging gateway"),
              std::string::npos);
}

TEST(GatewayCmd, FindGatewayPids_ReturnsVector) {
    auto pids = find_gateway_pids();
    // Unconditionally safe — the test does not assume gateway is
    // running; we just assert the API returns.
    (void)pids;
    SUCCEED();
}

TEST(GatewayCmd, SystemdServiceInstalled_SafeOnAnyHost) {
    // This probe is filesystem-based and should never throw.
    (void)systemd_service_installed();
    SUCCEED();
}

TEST(GatewayCmd, Status_Returns0) {
    char self[] = "hermes";
    char sub[] = "gateway";
    char status[] = "status";
    char* argv[] = {self, sub, status};
    EXPECT_EQ(run(3, argv), 0);
}

TEST(GatewayCmd, UnknownSubcommand_Errors) {
    char self[] = "hermes";
    char sub[] = "gateway";
    char bad[] = "xyzzy";
    char* argv[] = {self, sub, bad};
    EXPECT_EQ(run(3, argv), 1);
}

TEST(GatewayCmd, HelpPrintsUsage) {
    char self[] = "hermes";
    char sub[] = "gateway";
    char help[] = "--help";
    char* argv[] = {self, sub, help};
    EXPECT_EQ(run(3, argv), 0);
}
