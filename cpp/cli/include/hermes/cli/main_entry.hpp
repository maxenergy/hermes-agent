// hermes CLI entry point — dispatches subcommands (chat, gateway, setup,
// model, tools, skills, doctor, status, config, logs, cron, profile,
// version, update, uninstall).
#pragma once

#include <string>

namespace hermes::cli {

int main_entry(int argc, char* argv[]);

// Individual subcommand entry points — exposed for testing.
int cmd_version();
int cmd_doctor();
int cmd_status();
int cmd_model();
int cmd_tools();
int cmd_config(int argc, char* argv[]);
int cmd_gateway(int argc, char* argv[]);
int cmd_setup();
int cmd_skills();
int cmd_logs();
int cmd_cron();
int cmd_profile(int argc, char* argv[]);
int cmd_pairing(int argc, char* argv[]);
int cmd_update();
int cmd_uninstall();

// Version string constant.
inline constexpr const char* kVersionString = "hermes-cpp 0.0.1";

}  // namespace hermes::cli
