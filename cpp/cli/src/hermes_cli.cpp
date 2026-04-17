#include "hermes/cli/hermes_cli.hpp"

#include "hermes/agent/ai_agent.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/auth/codex_oauth.hpp"
#include "hermes/auth/qwen_client.hpp"
#include "hermes/cli/clipboard.hpp"
#include "hermes/cli/commands.hpp"
#include "hermes/cli/cron_cmd.hpp"
#include "hermes/cli/display.hpp"
#include "hermes/cli/image_attachment.hpp"
#include "hermes/cli/skin_engine.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/core/platform/subprocess.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/skills/skill_utils.hpp"
#include "hermes/core/path.hpp"
#include "hermes/state/memory_store.hpp"
#include "hermes/state/session_db.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>

#if !defined(_WIN32)
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(HERMES_HAS_READLINE)
#include <readline/history.h>
#include <readline/readline.h>
#include <cstdlib>
#endif
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
#if defined(HERMES_HAS_READLINE)
    // When stdin is a TTY, GNU readline gives us arrow-key editing,
    // Ctrl+A/E/U/W, ↑↓ history scroll and a ~/.hermes/history file.
    // When stdin is piped (tests, echo | hermes chat) we fall back to
    // std::getline so scripted input still works verbatim.
    const bool use_readline = ::isatty(STDIN_FILENO) != 0;
    std::string history_path;
    if (use_readline) {
        try {
            auto home = hermes::core::path::get_hermes_home();
            std::error_code ec;
            std::filesystem::create_directories(home, ec);
            history_path = (home / "input_history").string();
            ::using_history();
            ::read_history(history_path.c_str());
            ::stifle_history(1000);
        } catch (...) { /* non-fatal */ }
    }
#else
    const bool use_readline = false;
#endif

    std::string line;
    while (true) {
        const auto& skin = get_active_skin();
        std::string prompt_text;
        prompt_text += skin.colors.banner_accent;
        prompt_text += skin.branding.prompt_symbol;
        prompt_text += " ";
        prompt_text += skin.colors.banner_text;

#if defined(HERMES_HAS_READLINE)
        if (use_readline) {
            char* raw = ::readline(prompt_text.c_str());
            if (!raw) break;  // EOF (Ctrl-D)
            line = raw;
            std::free(raw);
        } else
#endif
        {
            std::cout << prompt_text << std::flush;
            if (!std::getline(std::cin, line)) break;
        }
        if (line.empty()) continue;

        // Multi-line: detect trailing backslash and continue reading.
        while (!line.empty() && line.back() == '\\') {
            line.pop_back();
            std::string continuation;
#if defined(HERMES_HAS_READLINE)
            if (use_readline) {
                char* raw = ::readline("... ");
                if (!raw) break;
                continuation = raw;
                std::free(raw);
            } else
#endif
            {
                std::cout << "... " << std::flush;
                if (!std::getline(std::cin, continuation)) break;
            }
            line += "\n" + continuation;
        }

        // Store in input history (cap at 100).
        input_history_.push_back(line);
        if (input_history_.size() > 100) {
            input_history_.erase(input_history_.begin());
        }
#if defined(HERMES_HAS_READLINE)
        if (use_readline && !line.empty()) {
            ::add_history(line.c_str());
            if (!history_path.empty()) {
                ::append_history(1, history_path.c_str());
            }
        }
#endif

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

    // Codex (ChatGPT) OAuth path.  The actual wire protocol is brokered
    // by CLIProxyAPI — a local proxy that holds the Codex OAuth session
    // and exposes an OpenAI-compatible /v1 surface (see
    // ~/.codex/config.toml :: [model_providers.cliproxy]).  Hermes hits
    // the proxy with CLIPROXY_API_KEY; direct ChatGPT backend access is
    // blocked by Cloudflare for non-browser clients.  If the proxy key
    // is absent we still record who the user *meant* to use so
    // ensure_agent's diagnostic message names Codex rather than
    // "generic openai missing key".
    if (provider == "openai-codex" || provider == "codex") {
        std::string base_url = "http://127.0.0.1:8993/v1";
        if (cfg.contains("base_url") && cfg["base_url"].is_string() &&
            !cfg["base_url"].get<std::string>().empty()) {
            base_url = cfg["base_url"].get<std::string>();
        }
        std::string token;
        if (auto* k = std::getenv("CLIPROXY_API_KEY")) token = k;
        if (token.empty()) {
            // Best-effort fallback to the Codex OAuth access_token —
            // Cloudflare will reject it on chatgpt.com but at least the
            // user sees a real HTTP error instead of "no credentials".
            auto creds = hermes::auth::load_codex_credentials();
            if (creds) {
                if (!creds->access_token.empty()) token = creds->access_token;
                else if (!creds->api_key.empty()) token = creds->api_key;
            }
        }
        if (token.empty()) return nullptr;
        auto client = std::make_unique<hermes::llm::OpenAIClient>(
            transport, token, base_url);
        client->set_provider_name("openai-codex");
        client->set_force_stream(true);
        return client;
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
    // reasoning_effort: accept either an int (0..3) or the Codex-style
    // strings "none"|"low"|"medium"|"high".  Silently clamps unknown
    // values to 0.
    if (config_.contains("reasoning_effort")) {
        const auto& v = config_["reasoning_effort"];
        if (v.is_number_integer()) {
            acfg.reasoning_effort = v.get<int>();
        } else if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (s == "low") acfg.reasoning_effort = 1;
            else if (s == "medium") acfg.reasoning_effort = 2;
            else if (s == "high") acfg.reasoning_effort = 3;
            else acfg.reasoning_effort = 0;
        }
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

    // Persistent curated memory — MEMORY.md / USER.md under
    // <HERMES_HOME>/memories/.  The agent-level `memory` tool (see
    // AIAgent::handle_memory) writes here synchronously and the
    // system-prompt snapshot is rebuilt on the next session.
    if (!memory_store_) {
        memory_store_ = std::make_unique<hermes::state::MemoryStore>();
    }

    agent_ = std::make_unique<hermes::agent::AIAgent>(
        std::move(acfg), llm_client_.get(),
        /*session_db=*/nullptr,
        /*context_engine=*/nullptr,
        /*memory=*/nullptr,
        prompt_builder_.get(),
        std::move(dispatcher),
        std::move(schemas));
    agent_->set_memory_store(memory_store_.get());
}

std::string HermesCLI::query(const std::string& message) {
    ensure_agent();

    // Consume any image attachment queued by /paste or /image.  On any
    // error we clear the pending path (single-shot semantics) and fall
    // back to text-only so the turn still proceeds.  This runs BEFORE
    // the no-provider bailout so the user still sees a helpful warning
    // when e.g. the file they pasted was deleted mid-turn.
    std::optional<hermes::llm::Message> image_user_turn;
    if (!pending_image_path_.empty()) {
        auto attach = build_image_user_message(message, pending_image_path_);
        switch (attach.error) {
            case AttachmentError::Ok:
                image_user_turn = std::move(attach.message);
                break;
            case AttachmentError::NotFound:
                std::cout << "[image missing: " << pending_image_path_
                          << "]\n";
                break;
            case AttachmentError::TooLarge:
                std::cout << "[image too large (>20MB): "
                          << pending_image_path_ << "]\n";
                break;
            case AttachmentError::ReadFailed:
                std::cout << "[image read failed: " << pending_image_path_
                          << " — " << attach.detail << "]\n";
                break;
            case AttachmentError::EmptyPath:
                // Unreachable given the outer guard, but kept explicit so
                // future callers don't silently drop an enumerant.
                break;
        }
        pending_image_path_.clear();
    }

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

        // If we built a multimodal user turn, append it to the history
        // and pass an empty user_message so AIAgent::run() doesn't tack
        // on a second text-only User message.  The text content already
        // lives inside the first content block.
        hermes::agent::ConversationResult result;
        if (image_user_turn) {
            history.push_back(std::move(*image_user_turn));
            result = agent_->run_conversation(std::string{}, std::nullopt, history);
        } else {
            result = agent_->run_conversation(message, std::nullopt, history);
        }
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

        // Persist to SessionDB so /resume can restore this conversation.
        ensure_session_id();
        persist_turn(message, text);
        if (session_db_ && !session_id_.empty()) {
            try {
                session_db_->add_tokens(session_id_,
                                        result.usage.input_tokens,
                                        result.usage.output_tokens,
                                        0.0);
            } catch (...) {}
        }

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
    else if (canonical == "clear")    { handle_clear(); }
    else if (canonical == "history")  { handle_history(); }
    else if (canonical == "save")     { handle_save(args); }
    else if (canonical == "exit" || canonical == "quit") {
        std::cout << "Goodbye!\n";
    }
    else if (canonical == "model")    { handle_model(args); }
    else if (canonical == "usage")    { handle_usage(); }
    else if (canonical == "status")   { handle_status(); }
    else if (canonical == "commands") { handle_commands(); }
    else if (canonical == "skills")   { handle_skills(); }
    else if (canonical == "tools")    { handle_tools(); }
    else if (canonical == "toolsets") { handle_toolsets(); }
    else if (canonical == "cron")     { handle_cron(args); }
    else if (canonical == "browser")  { handle_browser(args); }
    else if (canonical == "plugins")  { handle_plugins(); }
    else if (canonical == "compress") { handle_compress(); }
    else if (canonical == "verbose")  { handle_verbose(); }
    else if (canonical == "config")   { handle_config(); }
    else if (canonical == "statusbar"){ handle_statusbar(args); }
    else if (canonical == "skin")     { handle_skin_cmd(args); }
    else if (canonical == "personality") { handle_personality(args); }
    else if (canonical == "voice")    { handle_voice(args); }
    else if (canonical == "reasoning"){ handle_reasoning(args); }
    else if (canonical == "fast")     { handle_fast(); }
    else if (canonical == "yolo")     { handle_yolo(); }
    else if (canonical == "title")    { handle_title(args); }
    else if (canonical == "provider") { handle_provider(args); }
    else if (canonical == "insights") { handle_insights(); }
    else if (canonical == "platforms"){ handle_platforms(); }
    else if (canonical == "sessions") { handle_sessions(); }
    else if (canonical == "resume")   { handle_resume(args); }
    else if (canonical == "continue") { handle_continue(); }
    else if (canonical == "paste")    { handle_paste(); }
    else if (canonical == "image")    { handle_image(args); }
    else if (canonical == "prompt") {
        // Dump the live system prompt snapshot so the user can verify
        // which guidance blocks (memory, enforcement, platform hints…)
        // actually reached the model on this session.
        ensure_agent();
        if (!agent_) {
            std::cout << "(agent not constructed — no provider configured)\n";
        } else {
            const auto& msgs = agent_->messages();
            bool found = false;
            for (const auto& m : msgs) {
                if (m.role == hermes::llm::Role::System) {
                    std::cout << m.content_text << "\n";
                    found = true;
                    break;
                }
            }
            if (!found) {
                // No turn has run yet — rebuild the snapshot from the
                // current builder/memory state so /prompt works before
                // the first message.
                hermes::agent::PromptContext ctx;
                ctx.platform = "cli";
                if (config_.contains("model") && config_["model"].is_string()) {
                    ctx.model = config_["model"].get<std::string>();
                }
                if (memory_store_) {
                    try {
                        ctx.memory_entries = memory_store_->read_all(
                            hermes::state::MemoryFile::Agent);
                        ctx.user_entries = memory_store_->read_all(
                            hermes::state::MemoryFile::User);
                    } catch (...) {}
                }
                if (prompt_builder_) {
                    std::cout << prompt_builder_->build_system_prompt(ctx)
                              << "\n";
                } else {
                    std::cout << "(no prompt builder)\n";
                }
            }
        }
    }
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

// ── Session resume plumbing ──────────────────────────────────────────────

void HermesCLI::ensure_session_db() {
    if (session_db_) return;
    try {
        session_db_ = std::make_unique<hermes::state::SessionDB>();
    } catch (const std::exception& e) {
        std::cerr << "[warn] session db unavailable: " << e.what() << "\n";
    }
}

void HermesCLI::ensure_session_id() {
    if (!session_id_.empty()) return;
    ensure_session_db();
    if (!session_db_) return;
    std::string model;
    if (config_.contains("model") && config_["model"].is_string()) {
        model = config_["model"].get<std::string>();
    }
    try {
        session_id_ = session_db_->create_session("cli", model,
                                                  nlohmann::json::object());
    } catch (const std::exception& e) {
        std::cerr << "[warn] create_session failed: " << e.what() << "\n";
    }
}

void HermesCLI::persist_turn(const std::string& user_msg,
                             const std::string& assistant_msg) {
    if (!session_db_ || session_id_.empty()) return;
    hermes::state::MessageRow u;
    u.session_id = session_id_;
    u.turn_index = static_cast<int>(history_.size()) - 2;  // user added first
    u.role = "user";
    u.content = user_msg;
    u.tool_calls = nlohmann::json::array();
    u.created_at = std::chrono::system_clock::now();
    try { session_db_->save_message(u); } catch (...) {}

    hermes::state::MessageRow a;
    a.session_id = session_id_;
    a.turn_index = static_cast<int>(history_.size()) - 1;
    a.role = "assistant";
    a.content = assistant_msg;
    a.tool_calls = nlohmann::json::array();
    a.created_at = std::chrono::system_clock::now();
    try { session_db_->save_message(a); } catch (...) {}
}

bool HermesCLI::resume_session(const std::string& id_or_name) {
    ensure_session_db();
    if (!session_db_) {
        std::cout << "Session database unavailable.\n";
        return false;
    }
    std::string resolved_id;
    // 1. Exact id hit.
    if (session_db_->get_session(id_or_name).has_value()) {
        resolved_id = id_or_name;
    } else {
        // 2. Prefix / title substring match across recent sessions.
        auto sessions = session_db_->list_sessions(200, 0);
        for (const auto& s : sessions) {
            if (s.id.rfind(id_or_name, 0) == 0 ||
                (s.title && s.title->find(id_or_name) != std::string::npos)) {
                resolved_id = s.id;
                break;
            }
        }
    }
    if (resolved_id.empty()) {
        std::cout << "No session matching '" << id_or_name << "'.\n";
        return false;
    }
    auto msgs = session_db_->get_messages(resolved_id);
    session_id_ = resolved_id;
    history_.clear();
    for (const auto& m : msgs) {
        history_.push_back({{"role", m.role}, {"content", m.content}});
    }
    std::cout << "Resumed session " << resolved_id << " ("
              << msgs.size() << " messages).\n";
    return true;
}

bool HermesCLI::continue_last_session() {
    ensure_session_db();
    if (!session_db_) return false;
    auto sessions = session_db_->list_sessions(1, 0);
    if (sessions.empty()) {
        std::cout << "No previous sessions found.\n";
        return false;
    }
    return resume_session(sessions[0].id);
}

void HermesCLI::handle_sessions() {
    ensure_session_db();
    if (!session_db_) {
        std::cout << "Session database unavailable.\n";
        return;
    }
    auto sessions = session_db_->list_sessions(20, 0);
    if (sessions.empty()) {
        std::cout << "No sessions yet.\n";
        return;
    }
    std::cout << "Recent sessions (use /resume <id-prefix>):\n";
    for (const auto& s : sessions) {
        auto t = std::chrono::system_clock::to_time_t(s.updated_at);
        std::tm tm = *std::localtime(&t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        std::cout << "  " << s.id.substr(0, 12) << "  " << buf
                  << "  tok=" << (s.input_tokens + s.output_tokens)
                  << "  " << (s.title ? *s.title : std::string("(untitled)"))
                  << "\n";
    }
}

void HermesCLI::handle_resume(const std::string& args) {
    if (args.empty()) {
        std::cout << "Usage: /resume <session-id-or-prefix>\n";
        return;
    }
    resume_session(args);
}

void HermesCLI::handle_continue() {
    continue_last_session();
}

// ── Session: /clear /history /save ───────────────────────────────────────

void HermesCLI::handle_clear() {
    // ANSI "erase entire display + home cursor" — same sequence the Python
    // side emits via `os.system("clear")` under POSIX.  Then reset state
    // exactly like /new so the user really does start fresh.
    std::cout << "\033[2J\033[H" << std::flush;
    handle_new();
}

void HermesCLI::handle_history() {
    if (history_.empty()) {
        std::cout << "(._.) No conversation history yet.\n";
        return;
    }
    std::cout << "\n+--------------------------------------------------+\n"
              << "|            (^_^) Conversation History            |\n"
              << "+--------------------------------------------------+\n";
    constexpr std::size_t preview_limit = 400;
    int visible_index = 0;
    for (const auto& msg : history_) {
        if (!msg.is_object()) continue;
        auto role = msg.value("role", std::string{});
        if (role != "user" && role != "assistant") continue;
        ++visible_index;
        auto content = msg.value("content", std::string{});
        std::string preview = content.substr(0, preview_limit);
        const char* suffix = content.size() > preview_limit ? "..." : "";
        std::cout << "\n  [" << (role == "user" ? "You" : "Hermes")
                  << " #" << visible_index << "]\n    "
                  << preview << suffix << "\n";
    }
    std::cout << "\n";
}

void HermesCLI::handle_save(const std::string& args) {
    if (history_.empty()) {
        std::cout << "(;_;) No conversation to save.\n";
        return;
    }
    std::filesystem::path out_path;
    if (!args.empty()) {
        out_path = args;
    } else {
        auto t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm = *std::localtime(&t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        out_path = std::string("hermes_conversation_") + buf + ".json";
    }
    nlohmann::json doc;
    doc["model"] = (config_.contains("model") && config_["model"].is_string())
                       ? config_["model"].get<std::string>()
                       : std::string("(default)");
    doc["session_id"] = session_id_;
    doc["messages"] = history_;
    try {
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream f(out_path);
        if (!f) {
            std::cout << "(x_x) Failed to open " << out_path.string()
                      << " for writing.\n";
            return;
        }
        f << doc.dump(2);
        std::cout << "(^_^)v Conversation saved to: " << out_path.string()
                  << "\n";
    } catch (const std::exception& e) {
        std::cout << "(x_x) Failed to save: " << e.what() << "\n";
    }
}

// ── Configuration: /config /statusbar /skin ─────────────────────────────

void HermesCLI::handle_config() {
    std::cout << "Current configuration:\n" << config_.dump(2) << "\n";
}

void HermesCLI::handle_statusbar(const std::string& args) {
    bool want;
    if (args.empty()) {
        bool current = false;
        if (config_.contains("display") && config_["display"].is_object() &&
            config_["display"].contains("show_statusbar")) {
            current = config_["display"].value("show_statusbar", false);
        }
        want = !current;
    } else if (args == "on" || args == "true" || args == "1") {
        want = true;
    } else if (args == "off" || args == "false" || args == "0") {
        want = false;
    } else {
        std::cout << "Usage: /statusbar [on|off]\n";
        return;
    }
    config_["display"]["show_statusbar"] = want;
    std::cout << "Status bar: " << (want ? "on" : "off") << "\n";
}

void HermesCLI::handle_skin_cmd(const std::string& args) {
    if (args.empty()) {
        std::string current = "default";
        if (config_.contains("cli") && config_["cli"].is_object() &&
            config_["cli"].contains("skin") &&
            config_["cli"]["skin"].is_string()) {
            current = config_["cli"]["skin"].get<std::string>();
        }
        std::cout << "Active skin: " << current << "\n"
                  << "Built-in skins:\n";
        for (const auto& [name, cfg] : builtin_skins()) {
            std::cout << "  " << std::left << std::setw(12) << name
                      << cfg.description << "\n";
        }
    } else {
        config_["cli"]["skin"] = args;
        try { set_active_skin(args); } catch (...) {}
        std::cout << "Skin set to: " << args << "\n";
    }
}

// ── Tools & Skills: /toolsets /cron /browser /plugins ───────────────────

void HermesCLI::handle_toolsets() {
    const auto& ts = hermes::tools::toolsets();
    std::cout << "Available toolsets:\n";
    for (const auto& [name, def] : ts) {
        std::cout << "  " << std::left << std::setw(14) << name
                  << "— " << def.description << "\n";
    }
}

void HermesCLI::handle_cron(const std::string& args) {
    // Split args into whitespace-separated tokens.
    std::vector<std::string> tokens;
    {
        std::istringstream iss(args);
        std::string tok;
        while (iss >> tok) tokens.push_back(std::move(tok));
    }
    if (tokens.empty()) {
        (void)hermes::cli::cron_cmd::cmd_list({});
        return;
    }
    const std::string& sub = tokens[0];
    std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
    int rc = 0;
    if (sub == "list")            rc = hermes::cli::cron_cmd::cmd_list(rest);
    else if (sub == "status")     rc = hermes::cli::cron_cmd::cmd_status(rest);
    else if (sub == "pause")      rc = hermes::cli::cron_cmd::cmd_pause(rest);
    else if (sub == "resume")     rc = hermes::cli::cron_cmd::cmd_resume(rest);
    else if (sub == "remove")     rc = hermes::cli::cron_cmd::cmd_remove(rest);
    else if (sub == "run" || sub == "run-once") {
        rc = hermes::cli::cron_cmd::cmd_run_once(rest);
    }
    else if (sub == "enable")     rc = hermes::cli::cron_cmd::cmd_enable(rest, true);
    else if (sub == "disable")    rc = hermes::cli::cron_cmd::cmd_enable(rest, false);
    else if (sub == "install")    rc = hermes::cli::cron_cmd::cmd_install(rest);
    else if (sub == "uninstall")  rc = hermes::cli::cron_cmd::cmd_uninstall(rest);
    else if (sub == "tick")       rc = hermes::cli::cron_cmd::cmd_tick(rest);
    else {
        std::cout << "Usage: /cron [list|status|pause|resume|remove|run|"
                     "enable|disable|install|uninstall|tick] [args...]\n";
        return;
    }
    (void)rc;
}

void HermesCLI::handle_browser(const std::string& args) {
    // CDP browser bridge is not wired in the C++ CLI runtime yet; the
    // command surfaces a graceful stub so /help still advertises it and
    // users get a clear pointer to the Python CLI path.
    const std::string sub = args.empty() ? std::string("status") : args;
    if (sub == "status") {
        std::cout << "Browser bridge: disconnected\n"
                  << "  (CDP attach is available via the Python CLI;"
                     " the C++ REPL exposes a stub only.)\n";
    } else if (sub == "connect") {
        std::cout << "Browser connect is not available in this CLI context."
                     " Launch Chrome with --remote-debugging-port=9222 and"
                     " use the Python CLI.\n";
    } else if (sub == "disconnect") {
        std::cout << "Browser disconnect: no active session.\n";
    } else {
        std::cout << "Usage: /browser [connect|disconnect|status]\n";
    }
}

void HermesCLI::handle_plugins() {
    // Enumerate <HERMES_HOME>/plugins/* (one directory per plugin).  Full
    // install / update / remove flows still live in `hermes plugins`.
    namespace fs = std::filesystem;
    fs::path plugins_dir = hermes::core::path::get_hermes_home() / "plugins";
    std::error_code ec;
    if (!fs::exists(plugins_dir, ec)) {
        std::cout << "No plugins installed (" << plugins_dir.string() << ").\n";
        return;
    }
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(plugins_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto name = entry.path().filename().string();
        if (!name.empty() && name[0] != '.') names.push_back(name);
    }
    if (names.empty()) {
        std::cout << "No plugins installed.\n";
        return;
    }
    std::sort(names.begin(), names.end());
    std::cout << "Installed plugins (" << names.size() << "):\n";
    for (const auto& n : names) std::cout << "  " << n << "\n";
}

// ── Info: /paste /image ──────────────────────────────────────────────────

namespace {

// Generate a reasonably-unique temp path for a pasted image.  We don't
// need cryptographic strength — just avoid collisions inside /tmp.
std::filesystem::path make_paste_tmp_path() {
    auto t = std::chrono::system_clock::now().time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t)
                      .count();
    std::ostringstream name;
#if defined(_WIN32)
    name << "hermes-paste-" << micros << ".png";
#else
    name << "hermes-paste-" << micros << "-" << ::getpid() << ".png";
#endif
    return std::filesystem::temp_directory_path() / name.str();
}

}  // namespace

void HermesCLI::handle_paste() {
    auto tmp = make_paste_tmp_path();
    namespace platform = hermes::core::platform;

    auto try_tool = [&](std::vector<std::string> argv) -> bool {
        platform::SubprocessOptions o;
        o.argv = std::move(argv);
        o.timeout = std::chrono::milliseconds{3000};
        auto res = platform::run_capture(o);
        if (!res.spawn_error.empty() || res.exit_code != 0 ||
            res.stdout_text.empty()) {
            return false;
        }
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(res.stdout_text.data(),
                static_cast<std::streamsize>(res.stdout_text.size()));
        return static_cast<bool>(f);
    };

    bool captured = false;
    captured = try_tool({"xclip", "-selection", "clipboard",
                         "-t", "image/png", "-o"});
    if (!captured) {
        captured = try_tool({"wl-paste", "--type", "image/png"});
    }
    if (!captured) {
        std::cout << "(x_x) No image on the clipboard."
                     " Install xclip/wl-paste or use /image <path>.\n";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return;
    }
    pending_image_path_ = tmp.string();
    std::cout << "Image captured from clipboard: " << pending_image_path_
              << "\n"
              << "  Type your prompt on the next line and it will be sent"
                 " with the image attached.\n";
}

void HermesCLI::handle_image(const std::string& args) {
    if (args.empty()) {
        std::cout << "Usage: /image <path-to-image>\n";
        return;
    }
    std::filesystem::path p = args;
    if (!args.empty() && args.front() == '~') {
        if (auto* home = std::getenv("HOME")) {
            p = std::filesystem::path(home) / args.substr(1);
        }
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        std::cout << "(x_x) File not found: " << p.string() << "\n";
        return;
    }
    pending_image_path_ = std::filesystem::absolute(p, ec).string();
    std::cout << "Image queued for the next turn: " << pending_image_path_
              << "\n"
              << "  Type your prompt on the next line and it will be sent"
                 " with the image attached.\n";
}

}  // namespace hermes::cli
