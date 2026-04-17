// HermesCLI — main interactive REPL orchestrator.
//
// Owns the AIAgent, SessionDB, and history.  Dispatches slash commands via
// process_command() and forwards plain text to the agent.
#pragma once

#include "hermes/cli/skin_engine.hpp"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::agent { class AIAgent; class PromptBuilder; }
namespace hermes::llm { class LlmClient; }
namespace hermes::state { class SessionDB; class MemoryStore; }

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

    // Resume API — load a previous session's messages into the REPL state.
    // Returns true if the session was found and loaded.
    bool resume_session(const std::string& session_id_or_name);
    bool continue_last_session();

    // Visible for testing.
    void show_banner();
    void show_help();

private:
    void handle_new();
    void handle_reset();
    void handle_clear();
    void handle_history();
    void handle_save(const std::string& args);
    void handle_model(const std::string& args);
    void handle_skills();
    void handle_usage();
    void handle_compress();
    void handle_status();
    void handle_commands();
    void handle_tools();
    void handle_toolsets();
    void handle_cron(const std::string& args);
    void handle_browser(const std::string& args);
    void handle_plugins();
    void handle_verbose();
    void handle_config();
    void handle_statusbar(const std::string& args);
    void handle_skin_cmd(const std::string& args);
    void handle_personality(const std::string& args);
    void handle_voice(const std::string& args);
    void handle_reasoning(const std::string& args);
    void handle_fast();
    void handle_yolo();
    void handle_title(const std::string& args);
    void handle_provider(const std::string& args);
    void handle_insights();
    void handle_platforms();
    void handle_sessions();
    void handle_resume(const std::string& args);
    void handle_continue();
    void handle_paste();
    void handle_image(const std::string& args);

    void ensure_session_db();
    void ensure_session_id();
    void persist_turn(const std::string& user_msg,
                      const std::string& assistant_msg);

    nlohmann::json config_;
    std::string session_id_;
    std::vector<nlohmann::json> history_;  // lightweight; full Message in agent
    std::vector<std::string> input_history_;  // last 100 inputs for /retry
    std::unique_ptr<hermes::state::SessionDB> session_db_;
    int64_t total_input_tokens_ = 0;
    int64_t total_output_tokens_ = 0;
    bool verbose_ = false;
    bool yolo_ = false;
    double temperature_ = 1.0;
    // Attachment queued by /paste or /image for the next user message.
    // Empty when no attachment is pending.  Format: absolute path to an
    // image file on disk (PNG/JPEG).  Consumed + cleared when the next
    // plain-text message is dispatched to the agent.
    std::string pending_image_path_;

    // Lazily constructed on first plain-text input so the cost is only paid
    // when the user actually wants to chat (not for /status, /tools, etc.).
    std::unique_ptr<hermes::llm::LlmClient> llm_client_;
    std::unique_ptr<hermes::agent::PromptBuilder> prompt_builder_;
    std::unique_ptr<hermes::state::MemoryStore> memory_store_;
    std::unique_ptr<hermes::agent::AIAgent> agent_;
    void ensure_agent();
};

}  // namespace hermes::cli
