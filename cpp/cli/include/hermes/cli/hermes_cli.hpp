// HermesCLI — main interactive REPL orchestrator.
//
// Owns the AIAgent, SessionDB, and history.  Dispatches slash commands via
// process_command() and forwards plain text to the agent.
#pragma once

#include "hermes/cli/skin_engine.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::cli {

class HermesCLI {
public:
    HermesCLI();
    ~HermesCLI();

    HermesCLI(const HermesCLI&) = delete;
    HermesCLI& operator=(const HermesCLI&) = delete;

    // Main entry point — interactive REPL loop.
    void run();

    // Single query mode (non-interactive).
    std::string query(const std::string& message);

    // Command dispatch — called from REPL when input starts with '/'.
    // Returns true if the input was a recognized command.
    bool process_command(const std::string& input);

    // Visible for testing.
    void show_banner();
    void show_help();

private:
    void handle_new();
    void handle_reset();
    void handle_model(const std::string& args);
    void handle_skills();
    void handle_usage();
    void handle_compress();
    void handle_status();
    void handle_commands();
    void handle_tools();
    void handle_verbose();
    void handle_personality(const std::string& args);
    void handle_voice(const std::string& args);
    void handle_reasoning(const std::string& args);
    void handle_fast();
    void handle_yolo();
    void handle_title(const std::string& args);
    void handle_provider(const std::string& args);
    void handle_insights();
    void handle_platforms();

    nlohmann::json config_;
    std::string session_id_;
    std::vector<nlohmann::json> history_;  // lightweight; full Message in agent
    std::vector<std::string> input_history_;  // last 100 inputs for /retry
    int64_t total_input_tokens_ = 0;
    int64_t total_output_tokens_ = 0;
    bool verbose_ = false;
    bool yolo_ = false;
    double temperature_ = 1.0;
};

}  // namespace hermes::cli
