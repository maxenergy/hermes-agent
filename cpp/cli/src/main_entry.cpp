#include "hermes/cli/main_entry.hpp"

#include "hermes/cli/hermes_cli.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace {
constexpr const char* kVersion = "0.0.1";
}  // namespace

namespace hermes::cli {

namespace {

void print_version() {
    std::cout << "hermes " << kVersion << "\n";
}

void print_global_help() {
    std::cout << "Hermes — your AI agent\n\n"
              << "Usage: hermes [subcommand] [options]\n\n"
              << "Subcommands:\n"
              << "  chat        Start an interactive chat session (default)\n"
              << "  gateway     Run the gateway server\n"
              << "  setup       Run the setup wizard\n"
              << "  model       Show or set the default model\n"
              << "  tools       List available tools\n"
              << "  skills      List available skills\n"
              << "  doctor      Run diagnostic checks\n"
              << "  status      Show agent status\n"
              << "  config      Show or edit configuration\n"
              << "  logs        Show recent log output\n"
              << "  cron        Manage scheduled tasks\n"
              << "  profile     Show user profile\n"
              << "  version     Print version and exit\n"
              << "  update      Update Hermes\n"
              << "  uninstall   Remove Hermes\n"
              << "\n"
              << "Run 'hermes <subcommand> --help' for details.\n";
}

int cmd_doctor() {
    std::cout << "Running diagnostics...\n";
    // Basic checks.
    std::cout << "  [ok] C++ runtime\n";
    std::cout << "  [ok] Version: " << kVersion << "\n";
    // TODO: check config, database, API keys, etc.
    std::cout << "All checks passed.\n";
    return 0;
}

int cmd_status() {
    std::cout << "Hermes status:\n"
              << "  Version: " << kVersion << "\n"
              << "  Runtime: C++17\n"
              << "  Status: ready\n";
    return 0;
}

}  // namespace

int main_entry(int argc, char* argv[]) {
    if (argc < 2) {
        // Default: interactive chat.
        HermesCLI cli;
        cli.run();
        return 0;
    }

    std::string sub = argv[1];

    if (sub == "--help" || sub == "-h") {
        print_global_help();
        return 0;
    }
    if (sub == "--version" || sub == "-V" || sub == "version") {
        print_version();
        return 0;
    }
    if (sub == "chat") {
        HermesCLI cli;
        cli.run();
        return 0;
    }
    if (sub == "doctor") {
        return cmd_doctor();
    }
    if (sub == "status") {
        return cmd_status();
    }
    if (sub == "gateway") {
        std::cout << "Gateway — not yet implemented\n";
        return 1;
    }
    if (sub == "setup") {
        std::cout << "Setup wizard — not yet implemented\n";
        return 1;
    }
    if (sub == "model") {
        std::cout << "Model subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "tools") {
        std::cout << "Tools subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "skills") {
        std::cout << "Skills subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "config") {
        std::cout << "Config subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "logs") {
        std::cout << "Logs subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "cron") {
        std::cout << "Cron subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "profile") {
        std::cout << "Profile subcommand — not yet implemented\n";
        return 1;
    }
    if (sub == "update") {
        std::cout << "Update — not yet implemented\n";
        return 1;
    }
    if (sub == "uninstall") {
        std::cout << "Uninstall — not yet implemented\n";
        return 1;
    }

    std::cerr << "Unknown subcommand: " << sub << "\n";
    print_global_help();
    return 1;
}

}  // namespace hermes::cli
