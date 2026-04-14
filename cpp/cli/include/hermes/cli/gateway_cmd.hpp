// C++17 port of hermes_cli/gateway.py — `hermes gateway` subcommand.
//
// The Python version bundles ~2700 LOC of systemd/launchd service
// management, manual run, supervision, and setup wizard.  This C++
// port focuses on the operator-facing surface:
//
//   - `start|stop|restart|status|reconnect` — manage a running
//      gateway process (systemd --user service when available,
//      PID-file fallback otherwise).
//   - `logs [--follow] [--tail N]`        — view gateway log tail.
//   - `install|uninstall`                 — provision/remove the
//      systemd --user unit.
//   - `pids`                              — print PIDs of gateway
//      processes matching known patterns.
//
// Like other ported modules, the interactive setup wizard path
// (`hermes gateway setup`) is deferred to the existing setup wizard.
#pragma once

#include <string>
#include <vector>

namespace hermes::cli::gateway_cmd {

int run(int argc, char* argv[]);

// Subcommand handlers.
int cmd_start(const std::vector<std::string>& argv);
int cmd_stop(const std::vector<std::string>& argv);
int cmd_status(const std::vector<std::string>& argv);
int cmd_restart(const std::vector<std::string>& argv);
int cmd_reconnect(const std::vector<std::string>& argv);
int cmd_logs(const std::vector<std::string>& argv);
int cmd_install(const std::vector<std::string>& argv);
int cmd_uninstall(const std::vector<std::string>& argv);
int cmd_pids(const std::vector<std::string>& argv);

// Pure helper — renders the systemd unit body used by install/uninstall.
std::string render_service_unit(const std::string& exec_path);

// Return the list of PIDs matching gateway-like processes.  Non-Linux
// platforms return empty.
std::vector<int> find_gateway_pids();

// Whether the systemd --user timer service is installed and active.
// Returns false on non-Linux.
bool systemd_service_installed();
bool systemd_service_active();

}  // namespace hermes::cli::gateway_cmd
