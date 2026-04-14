// C++17 port of hermes_cli/cron.py — `hermes cron` subcommand.
//
// Wraps the existing hermes::cron::JobStore with a fuller CLI
// (install/uninstall/list/status/enable/disable/run-once/create/edit/
// pause/resume/remove/tick) matching the Python surface.  The
// install/uninstall pieces provision a systemd --user service that
// runs `hermes cron run-once` on a fixed cadence — when systemd is
// unavailable, we fall back to a cron-style line in `crontab -l`.
#pragma once

#include <string>
#include <vector>

namespace hermes::cli::cron_cmd {

int run(int argc, char* argv[]);

// Subcommand handlers — unit-testable.
int cmd_list(const std::vector<std::string>& argv);
int cmd_status(const std::vector<std::string>& argv);
int cmd_tick(const std::vector<std::string>& argv);
int cmd_run_once(const std::vector<std::string>& argv);
int cmd_pause(const std::vector<std::string>& argv);
int cmd_resume(const std::vector<std::string>& argv);
int cmd_remove(const std::vector<std::string>& argv);
int cmd_install(const std::vector<std::string>& argv);
int cmd_uninstall(const std::vector<std::string>& argv);
int cmd_enable(const std::vector<std::string>& argv, bool enable);

// Pure helper — assemble the systemd unit bodies (service + timer).
struct SystemdUnits {
    std::string service_body;
    std::string timer_body;
};
SystemdUnits render_systemd_units(const std::string& on_calendar);

}  // namespace hermes::cli::cron_cmd
