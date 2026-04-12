#include "hermes/cli/hermes_cli.hpp"

#include "hermes/cli/commands.hpp"
#include "hermes/cli/display.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

namespace hermes::cli {

HermesCLI::HermesCLI() {
    // Lightweight construction — no agent or session created yet.
    // Phase 13 focuses on command dispatch; agent integration is Phase 14+.
}

HermesCLI::~HermesCLI() = default;

void HermesCLI::run() {
    show_banner();
    std::string line;
    while (true) {
        const auto& skin = get_active_skin();
        std::cout << skin.colors.banner_accent
                  << skin.branding.prompt_symbol << " "
                  << skin.colors.banner_text
                  << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line[0] == '/') {
            if (!process_command(line)) {
                std::cout << "Unknown command: " << line
                          << "  (type /help for a list)\n";
            }
            // Check for exit.
            auto resolved = resolve_command(line.substr(1));
            if (resolved && (resolved->name == "exit" || resolved->name == "quit")) {
                break;
            }
            continue;
        }

        // Plain text → agent query (stub for Phase 13).
        std::cout << "[agent not wired yet] " << line << "\n";
    }
}

std::string HermesCLI::query(const std::string& message) {
    // Stub — will be wired to AIAgent in a later phase.
    return "[agent not wired yet] " + message;
}

bool HermesCLI::process_command(const std::string& input) {
    // Parse: "/cmd arg1 arg2 ..."
    std::string trimmed = input;
    if (!trimmed.empty() && trimmed[0] == '/') trimmed.erase(0, 1);
    if (trimmed.empty()) return false;

    // Split into command and args.
    auto space_pos = trimmed.find(' ');
    std::string cmd_name = trimmed.substr(0, space_pos);
    std::string args;
    if (space_pos != std::string::npos) {
        args = trimmed.substr(space_pos + 1);
    }

    auto def = resolve_command(cmd_name);
    if (!def) return false;

    const auto& canonical = def->name;

    // Dispatch on canonical name.
    if (canonical == "help")          { show_help(); }
    else if (canonical == "new")      { handle_new(); }
    else if (canonical == "reset")    { handle_reset(); }
    else if (canonical == "exit" || canonical == "quit") {
        std::cout << "Goodbye!\n";
    }
    else if (canonical == "model")    { handle_model(args); }
    else if (canonical == "usage")    { handle_usage(); }
    else if (canonical == "status")   { handle_status(); }
    else if (canonical == "commands") { handle_commands(); }
    else if (canonical == "skills")   { handle_skills(); }
    else if (canonical == "tools")    { handle_tools(); }
    else if (canonical == "compress") { handle_compress(); }
    else if (canonical == "verbose")  { handle_verbose(); }
    else if (canonical == "personality") { handle_personality(args); }
    else if (canonical == "voice")    { handle_voice(args); }
    else if (canonical == "reasoning"){ handle_reasoning(args); }
    else if (canonical == "fast")     { handle_fast(); }
    else if (canonical == "yolo")     { handle_yolo(); }
    else if (canonical == "title")    { handle_title(args); }
    else if (canonical == "provider") { handle_provider(args); }
    else {
        std::cout << "/" << canonical << " — not yet implemented\n";
    }

    return true;
}

void HermesCLI::show_banner() {
    const auto& skin = get_active_skin();
    const auto& c = skin.colors;
    std::cout << c.banner_border << "╔══════════════════════════════════════╗\n"
              << c.banner_border << "║ "
              << c.banner_title << skin.branding.agent_name
              << c.banner_border << "                              ║\n"
              << c.banner_border << "╚══════════════════════════════════════╝\n"
              << c.banner_text;
    if (!skin.branding.welcome.empty()) {
        std::cout << skin.branding.welcome << "\n";
    }
    std::cout << "Type /help for commands.\n\n";
}

void HermesCLI::show_help() {
    auto by_cat = commands_by_category();
    for (const auto& [cat, cmds] : by_cat) {
        std::cout << "\n  " << cat << ":\n";
        for (const auto& cmd : cmds) {
            std::string line = "    /" + cmd.name;
            if (!cmd.args_hint.empty()) line += " " + cmd.args_hint;
            // Pad to 28 chars.
            while (line.size() < 28) line += ' ';
            line += cmd.description;
            if (!cmd.aliases.empty()) {
                line += "  (alias:";
                for (const auto& a : cmd.aliases) line += " /" + a;
                line += ")";
            }
            std::cout << line << "\n";
        }
    }
    std::cout << "\n";
}

void HermesCLI::handle_new() {
    session_id_.clear();
    history_.clear();
    total_input_tokens_ = 0;
    total_output_tokens_ = 0;
    std::cout << "New session started.\n";
}

void HermesCLI::handle_reset() {
    history_.clear();
    total_input_tokens_ = 0;
    total_output_tokens_ = 0;
    std::cout << "Conversation history cleared.\n";
}

void HermesCLI::handle_model(const std::string& args) {
    if (args.empty()) {
        std::string current = "anthropic/claude-opus-4-6";
        if (config_.contains("model")) {
            current = config_["model"].get<std::string>();
        }
        std::cout << "Current model: " << current << "\n";
    } else {
        config_["model"] = args;
        std::cout << "Model set to: " << args << "\n";
    }
}

void HermesCLI::handle_skills() {
    std::cout << "Skills: (skill list not yet wired)\n";
}

void HermesCLI::handle_tools() {
    std::cout << "Tools: (tool list not yet wired)\n";
}

void HermesCLI::handle_usage() {
    std::cout << "Token usage this session:\n"
              << "  Input tokens:  " << total_input_tokens_ << "\n"
              << "  Output tokens: " << total_output_tokens_ << "\n"
              << "  Turns: " << history_.size() << "\n";
}

void HermesCLI::handle_compress() {
    std::cout << "Context compression — not yet implemented\n";
}

void HermesCLI::handle_status() {
    std::cout << "Session: " << (session_id_.empty() ? "(none)" : session_id_) << "\n"
              << "History length: " << history_.size() << "\n"
              << "Model: "
              << (config_.contains("model") ? config_["model"].get<std::string>()
                                            : "anthropic/claude-opus-4-6")
              << "\n";
}

void HermesCLI::handle_commands() {
    auto flat = commands_flat();
    std::cout << "All commands (" << command_registry().size() << "):\n";
    for (const auto& cmd : command_registry()) {
        std::cout << "  /" << cmd.name << " — " << cmd.description << "\n";
    }
}

void HermesCLI::handle_verbose() {
    std::cout << "Verbose mode toggled — not yet implemented\n";
}

void HermesCLI::handle_personality(const std::string& args) {
    if (args.empty()) {
        std::cout << "Personality: default\n";
    } else {
        std::cout << "Personality set to: " << args << " — not yet implemented\n";
    }
}

void HermesCLI::handle_voice(const std::string& args) {
    if (args.empty()) {
        std::cout << "Voice: default\n";
    } else {
        std::cout << "Voice set to: " << args << " — not yet implemented\n";
    }
}

void HermesCLI::handle_reasoning(const std::string& args) {
    std::cout << "Reasoning mode: " << (args.empty() ? "toggle" : args)
              << " — not yet implemented\n";
}

void HermesCLI::handle_fast() {
    std::cout << "Switched to fast model preset — not yet implemented\n";
}

void HermesCLI::handle_yolo() {
    std::cout << "Auto-approve mode toggled — not yet implemented\n";
}

void HermesCLI::handle_title(const std::string& args) {
    if (args.empty()) {
        std::cout << "Session title: (none)\n";
    } else {
        std::cout << "Title set to: " << args << "\n";
    }
}

void HermesCLI::handle_provider(const std::string& args) {
    if (args.empty()) {
        std::cout << "Provider: (default)\n";
    } else {
        std::cout << "Provider set to: " << args << " — not yet implemented\n";
    }
}

}  // namespace hermes::cli
