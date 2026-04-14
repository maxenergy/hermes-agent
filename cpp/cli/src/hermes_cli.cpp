#include "hermes/cli/hermes_cli.hpp"

#include "hermes/agent/ai_agent.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/auth/qwen_client.hpp"
#include "hermes/cli/commands.hpp"
#include "hermes/cli/display.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/skills/skill_utils.hpp"
#include "hermes/tools/clarify_tool.hpp"
#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/discover_tools.hpp"
#include "hermes/tools/homeassistant_tool.hpp"
#include "hermes/tools/memory_tool.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/tools/session_search_tool.hpp"
#include "hermes/tools/skills_tool.hpp"
#include "hermes/tools/todo_tool.hpp"
#include "hermes/tools/toolsets.hpp"

#include <cstdlib>
#include <memory>
#include <mutex>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace hermes::cli {

namespace {
// Forward declaration — definition below.
std::unique_ptr<hermes::llm::LlmClient> make_client(const nlohmann::json& cfg);
}

HermesCLI::HermesCLI() {
    // Load persisted user config so /status, /model, /personality and friends
    // reflect what's on disk instead of an empty default.
    try {
        config_ = hermes::config::load_cli_config();
    } catch (...) {
        config_ = nlohmann::json::object();
    }

    // Register all built-in tools with the global ToolRegistry exactly once
    // per process — this is what makes file/terminal/web/etc. callable from
    // within the agent loop.
    static std::once_flag s_tools_inited;
    std::call_once(s_tools_inited, [this] {
        try {
            hermes::tools::discover_tools();
            auto& reg = hermes::tools::ToolRegistry::instance();
            hermes::tools::register_memory_tools(reg);
            hermes::tools::register_todo_tools(reg);
            hermes::tools::register_clarify_tools(reg);
            hermes::tools::register_skills_tools(reg);
            hermes::tools::register_session_search_tools(reg);
            hermes::tools::register_homeassistant_tools(reg);

            // Sub-agent factory for delegate_task / mixture_of_agents.
            // The sub-agent is a fresh QwenClient/OpenAIClient that runs a
            // single chat turn — no recursive tool calls (max_iterations=1)
            // to keep cost predictable.
            class CliSubAgent : public hermes::tools::AIAgent {
            public:
                explicit CliSubAgent(std::unique_ptr<hermes::llm::LlmClient> c)
                    : client_(std::move(c)) {}
                std::string run(const std::string& goal,
                                 const std::string& constraints) override {
                    if (!client_) return "{\"error\":\"no LLM client\"}";
                    hermes::llm::CompletionRequest req;
                    req.model = "coder-model";  // Qwen default; ignored by other providers
                    hermes::llm::Message sys;
                    sys.role = hermes::llm::Role::System;
                    sys.content_text = "You are a focused sub-agent. Complete the goal in one response. " + constraints;
                    hermes::llm::Message user;
                    user.role = hermes::llm::Role::User;
                    user.content_text = goal;
                    req.messages = {sys, user};
                    try {
                        auto r = client_->complete(req);
                        return r.assistant_message.content_text;
                    } catch (const std::exception& e) {
                        return std::string("{\"error\":\"") + e.what() + "\"}";
                    }
                }
            private:
                std::unique_ptr<hermes::llm::LlmClient> client_;
            };

            auto cfg_copy = config_;  // capture by value for the lambda
            hermes::tools::register_delegate_tools(
                [cfg_copy](const std::string& /*model*/)
                    -> std::unique_ptr<hermes::tools::AIAgent> {
                    return std::make_unique<CliSubAgent>(make_client(cfg_copy));
                });
        } catch (const std::exception& e) {
            std::cerr << "[warn] tool registration failed: " << e.what() << "\n";
        }
    });
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

        // Multi-line: detect trailing backslash and continue reading.
        while (!line.empty() && line.back() == '\\') {
            line.pop_back();
            std::string continuation;
            std::cout << "... " << std::flush;
            if (!std::getline(std::cin, continuation)) break;
            line += "\n" + continuation;
        }

        // Store in input history (cap at 100).
        input_history_.push_back(line);
        if (input_history_.size() > 100) {
            input_history_.erase(input_history_.begin());
        }

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

        // Plain text → agent query with spinner.
        Spinner spinner(skin);
        spinner.start("thinking");
        auto result = query(line);
        spinner.stop();

        // Print response with skin's response_label prefix.
        std::cout << skin.colors.response_border
                  << skin.branding.response_label
                  << skin.colors.banner_text
                  << result << "\n";
    }
}

namespace {

// Build an LlmClient based on config_["provider"].  Returns nullptr if no
// provider is configured / credentials missing.
std::unique_ptr<hermes::llm::LlmClient> make_client(const nlohmann::json& cfg) {
    auto* transport = hermes::llm::get_default_transport();
    std::string provider;
    if (cfg.contains("provider") && cfg["provider"].is_string()) {
        provider = cfg["provider"].get<std::string>();
    }

    if (provider == "qwen") {
        return std::make_unique<hermes::auth::QwenClient>(transport);
    }

    // Generic OpenAI-compatible: provider == "openai" / "openrouter" / etc.
    std::string base_url = "https://api.openai.com/v1";
    if (cfg.contains("base_url") && cfg["base_url"].is_string() &&
        !cfg["base_url"].get<std::string>().empty()) {
        base_url = cfg["base_url"].get<std::string>();
    }
    std::string api_key;
    if (auto* k = std::getenv("OPENAI_API_KEY")) api_key = k;
    if (api_key.empty() && cfg.contains("provider_api_key") &&
        cfg["provider_api_key"].is_string()) {
        api_key = cfg["provider_api_key"].get<std::string>();
    }
    if (api_key.empty() && provider != "qwen") return nullptr;
    return std::make_unique<hermes::llm::OpenAIClient>(transport, api_key, base_url);
}

}  // namespace

void HermesCLI::ensure_agent() {
    if (agent_) return;
    llm_client_ = make_client(config_);
    if (!llm_client_) return;  // query() will print a helpful error

    prompt_builder_ = std::make_unique<hermes::agent::PromptBuilder>();

    hermes::agent::AgentConfig acfg;
    if (config_.contains("model") && config_["model"].is_string()) {
        acfg.model = config_["model"].get<std::string>();
    }
    if (config_.contains("provider") && config_["provider"].is_string()) {
        acfg.provider = config_["provider"].get<std::string>();
    }
    acfg.platform = "cli";
    acfg.temperature = temperature_;
    acfg.max_iterations = 30;

    // Resolve enabled tools into ToolSchema list for the LLM.
    auto schemas = hermes::tools::ToolRegistry::instance().get_definitions();

    // Real dispatcher — routes every tool call through the global registry,
    // which now holds all built-in tool implementations.
    hermes::agent::ToolDispatcher dispatcher =
        [](const std::string& name, const nlohmann::json& args,
           const std::string& task_id) -> std::string {
            hermes::tools::ToolContext ctx;
            ctx.task_id = task_id;
            ctx.platform = "cli";
            return hermes::tools::ToolRegistry::instance().dispatch(name, args, ctx);
        };

    agent_ = std::make_unique<hermes::agent::AIAgent>(
        std::move(acfg), llm_client_.get(),
        /*session_db=*/nullptr,
        /*context_engine=*/nullptr,
        /*memory=*/nullptr,
        prompt_builder_.get(),
        std::move(dispatcher),
        std::move(schemas));
}

std::string HermesCLI::query(const std::string& message) {
    ensure_agent();
    if (!agent_) {
        return "[no provider configured — run `hermes login` or set "
               "OPENAI_API_KEY] " + message;
    }

    try {
        // Reuse history_ as conversation context across turns so the model
        // sees the running dialog (HermesCLI owns history; AIAgent is
        // session-less here).
        std::vector<hermes::llm::Message> history;
        for (const auto& h : history_) {
            if (!h.is_object()) continue;
            auto role = h.value("role", "user");
            hermes::llm::Message m;
            m.role = (role == "assistant") ? hermes::llm::Role::Assistant
                    : (role == "system")    ? hermes::llm::Role::System
                    : (role == "tool")      ? hermes::llm::Role::Tool
                                             : hermes::llm::Role::User;
            m.content_text = h.value("content", "");
            history.push_back(std::move(m));
        }

        auto result = agent_->run_conversation(message, std::nullopt, history);
        std::string text = result.final_response;
        // Qwen's thinking model can put the actual answer in reasoning when
        // the model decides to "think aloud" instead of emitting content.
        // Surface the reasoning so the user isn't left staring at an
        // empty bubble.
        if (text.empty() && !result.messages.empty()) {
            const auto& last = result.messages.back();
            if (last.reasoning && !last.reasoning->empty()) {
                text = "(thinking) " + *last.reasoning;
            }
        }

        // Persist this turn for subsequent /retry/history.
        history_.push_back({{"role", "user"}, {"content", message}});
        history_.push_back({{"role", "assistant"}, {"content", text}});
        total_input_tokens_ += result.usage.input_tokens;
        total_output_tokens_ += result.usage.output_tokens;

        return text.empty() ? "[empty response]" : text;
    } catch (const std::exception& e) {
        return std::string("[error] ") + e.what();
    }
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
    else if (canonical == "insights") { handle_insights(); }
    else if (canonical == "platforms"){ handle_platforms(); }
    else {
        std::cout << "/" << canonical << " is not available in this context\n";
    }

    return true;
}

void HermesCLI::show_banner() {
    const auto& skin = get_active_skin();
    const auto& c = skin.colors;
    std::cout << "\n"
              << c.banner_border << "  ╭─────────────────────────────────╮\n"
              << c.banner_border << "  │  "
              << c.banner_title << skin.branding.agent_name
              << " Agent v0.0.1"
              << c.banner_border << "              │\n"
              << c.banner_border << "  │  "
              << c.banner_dim << "C++17 Backend"
              << c.banner_border << "                  │\n"
              << c.banner_border << "  ╰─────────────────────────────────╯\n"
              << c.banner_text << "\n";
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
    input_history_.clear();
    total_input_tokens_ = 0;
    total_output_tokens_ = 0;
    std::cout << "New session started.\n";
}

void HermesCLI::handle_reset() {
    history_.clear();
    input_history_.clear();
    total_input_tokens_ = 0;
    total_output_tokens_ = 0;
    std::cout << "Conversation history cleared.\n";
}

void HermesCLI::handle_model(const std::string& args) {
    if (args.empty()) {
        std::string current = "anthropic/claude-opus-4-6";
        if (config_.contains("model") && config_["model"].is_string()) {
            current = config_["model"].get<std::string>();
        }
        std::cout << "Current model: " << current << "\n";
    } else {
        config_["model"] = args;
        std::cout << "Model set to: " << args << "\n";
    }
}

void HermesCLI::handle_skills() {
    auto skills = hermes::skills::iter_skill_index();
    if (skills.empty()) {
        std::cout << "No skills found.\n";
        return;
    }
    std::cout << "Available skills:\n";
    for (const auto& skill : skills) {
        std::string status = skill.enabled ? "" : " (disabled)";
        std::cout << "  " << std::left << std::setw(20) << skill.name
                  << skill.description << status << "\n";
    }
}

void HermesCLI::handle_tools() {
    const auto& ts = hermes::tools::toolsets();
    std::cout << "Available toolsets:\n";
    for (const auto& [name, def] : ts) {
        std::cout << "  " << name << " — " << def.description << "\n";
    }
}

void HermesCLI::handle_usage() {
    std::cout << "Token usage this session:\n"
              << "  Input tokens:  " << total_input_tokens_ << "\n"
              << "  Output tokens: " << total_output_tokens_ << "\n"
              << "  Turns: " << history_.size() << "\n";
}

void HermesCLI::handle_compress() {
    std::cout << "Compressing context...\n";
    if (history_.empty()) {
        std::cout << "No messages to compress.\n";
        return;
    }
    // Compress by keeping only the last 10 messages and a summary marker.
    std::size_t keep = 10;
    if (history_.size() > keep) {
        auto removed = history_.size() - keep;
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(removed));
        std::cout << "Compressed: removed " << removed
                  << " older messages, keeping " << keep << ".\n";
    } else {
        std::cout << "History is already compact (" << history_.size() << " messages).\n";
    }
}

void HermesCLI::handle_status() {
    std::cout << "Session: " << (session_id_.empty() ? "(none)" : session_id_) << "\n"
              << "History length: " << history_.size() << "\n"
              << "Model: "
              << ((config_.contains("model") && config_["model"].is_string())
                      ? config_["model"].get<std::string>()
                      : std::string("anthropic/claude-opus-4-6"))
              << "\n"
              << "Provider: "
              << ((config_.contains("provider") && config_["provider"].is_string())
                      ? config_["provider"].get<std::string>()
                      : std::string("(default)"))
              << "\n"
              << "Base URL: "
              << ((config_.contains("base_url") && config_["base_url"].is_string())
                      ? config_["base_url"].get<std::string>()
                      : std::string("(default)"))
              << "\n"
              << "Verbose: " << (verbose_ ? "on" : "off") << "\n"
              << "Yolo: " << (yolo_ ? "on" : "off") << "\n"
              << "Temperature: " << temperature_ << "\n";
}

void HermesCLI::handle_commands() {
    auto flat = commands_flat();
    std::cout << "All commands (" << command_registry().size() << "):\n";
    for (const auto& cmd : command_registry()) {
        std::cout << "  /" << cmd.name << " — " << cmd.description << "\n";
    }
}

void HermesCLI::handle_verbose() {
    verbose_ = !verbose_;
    config_["display"]["tool_progress_command"] = verbose_;
    std::cout << "Verbose mode: " << (verbose_ ? "on" : "off") << "\n";
}

void HermesCLI::handle_personality(const std::string& args) {
    if (args.empty()) {
        std::string current = "default";
        if (config_.contains("personality") && config_["personality"].is_string()) {
            current = config_["personality"].get<std::string>();
        }
        std::cout << "Personality: " << current << "\n";
    } else {
        config_["personality"] = args;
        std::cout << "Personality set to: " << args << "\n";
    }
}

void HermesCLI::handle_voice(const std::string& args) {
    if (args.empty()) {
        std::string current = "default";
        if (config_.contains("tts") && config_["tts"].contains("voice") &&
            config_["tts"]["voice"].is_string()) {
            current = config_["tts"]["voice"].get<std::string>();
        }
        std::cout << "Voice: " << current << "\n";
    } else {
        config_["tts"]["voice"] = args;
        std::cout << "Voice set to: " << args << "\n";
    }
}

void HermesCLI::handle_reasoning(const std::string& args) {
    if (args.empty()) {
        // Toggle: cycle 0 -> 1 -> 2 -> 3 -> 0.
        int current = 0;
        if (config_.contains("reasoning_effort") && config_["reasoning_effort"].is_number()) {
            current = config_["reasoning_effort"].get<int>();
        }
        current = (current + 1) % 4;
        config_["reasoning_effort"] = current;
        std::cout << "Reasoning effort set to: " << current << "\n";
    } else {
        try {
            int level = std::stoi(args);
            if (level < 0 || level > 3) {
                std::cout << "Reasoning level must be 0-3.\n";
                return;
            }
            config_["reasoning_effort"] = level;
            std::cout << "Reasoning effort set to: " << level << "\n";
        } catch (...) {
            std::cout << "Invalid reasoning level: " << args << " (expected 0-3)\n";
        }
    }
}

void HermesCLI::handle_fast() {
    // Toggle temperature between 0.0 and 1.0.
    if (temperature_ > 0.5) {
        temperature_ = 0.0;
    } else {
        temperature_ = 1.0;
    }
    config_["temperature"] = temperature_;
    std::cout << "Temperature set to: " << temperature_
              << (temperature_ < 0.5 ? " (fast/deterministic)" : " (creative)")
              << "\n";
}

void HermesCLI::handle_yolo() {
    yolo_ = !yolo_;
    config_["approval"]["auto_approve"] = yolo_;
    std::cout << "Auto-approve (yolo) mode: " << (yolo_ ? "on" : "off") << "\n";
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
        std::string current = "(default)";
        if (config_.contains("provider") && config_["provider"].is_string()) {
            current = config_["provider"].get<std::string>();
        }
        std::cout << "Provider: " << current << "\n";
    } else {
        config_["provider"] = args;
        std::cout << "Provider set to: " << args << "\n";
    }
}

void HermesCLI::handle_insights() {
    std::cout << "Session insights:\n"
              << "  Turns: " << history_.size() << "\n"
              << "  Input tokens:  " << total_input_tokens_ << "\n"
              << "  Output tokens: " << total_output_tokens_ << "\n"
              << "  Input history: " << input_history_.size() << " entries\n";
}

void HermesCLI::handle_platforms() {
    std::cout << "Connected platforms:\n"
              << "  CLI only\n";
}

}  // namespace hermes::cli
