// Tests for hermes::cli::cron_cmd.
#include "hermes/cli/cron_cmd.hpp"

#include <gtest/gtest.h>

using namespace hermes::cli::cron_cmd;

TEST(CronCmd, SystemdUnits_ServiceHasExecStart) {
    auto u = render_systemd_units("*:0/5");
    EXPECT_NE(u.service_body.find("[Service]"), std::string::npos);
    EXPECT_NE(u.service_body.find("ExecStart="), std::string::npos);
    EXPECT_NE(u.service_body.find("hermes cron tick"), std::string::npos);
}

TEST(CronCmd, SystemdUnits_TimerHasOnCalendar) {
    auto u = render_systemd_units("daily");
    EXPECT_NE(u.timer_body.find("[Timer]"), std::string::npos);
    EXPECT_NE(u.timer_body.find("OnCalendar=daily"), std::string::npos);
    EXPECT_NE(u.timer_body.find("Persistent=true"), std::string::npos);
}

TEST(CronCmd, SystemdUnits_TimerWantedBy) {
    auto u = render_systemd_units("*:0/1");
    EXPECT_NE(u.timer_body.find("[Install]"), std::string::npos);
    EXPECT_NE(u.timer_body.find("WantedBy=timers.target"),
              std::string::npos);
}

TEST(CronCmd, RunList_Empty_Quiet) {
    // `hermes cron list` with no jobs should return 0.  We drive it
    // via the argv-level entry so it runs against whatever the current
    // HERMES_HOME is — the conftest-style isolation is managed by the
    // surrounding Python suite; here we just check success.
    char self[] = "hermes";
    char sub[] = "cron";
    char list[] = "list";
    char* argv[] = {self, sub, list};
    int rc = run(3, argv);
    EXPECT_EQ(rc, 0);
}

TEST(CronCmd, UnknownSubcommand_ReturnsError) {
    char self[] = "hermes";
    char sub[] = "cron";
    char bogus[] = "floopy";
    char* argv[] = {self, sub, bogus};
    int rc = run(3, argv);
    EXPECT_EQ(rc, 1);
}

TEST(CronCmd, TickWithNoJobs_ReturnsZero) {
    char self[] = "hermes";
    char sub[] = "cron";
    char tick[] = "tick";
    char* argv[] = {self, sub, tick};
    int rc = run(3, argv);
    EXPECT_EQ(rc, 0);
}
