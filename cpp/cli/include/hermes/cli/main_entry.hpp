// hermes CLI entry point — dispatches subcommands (chat, gateway, setup,
// model, tools, skills, doctor, status, config, logs, cron, profile,
// version, update, uninstall).
#pragma once

namespace hermes::cli {

int main_entry(int argc, char* argv[]);

}  // namespace hermes::cli
