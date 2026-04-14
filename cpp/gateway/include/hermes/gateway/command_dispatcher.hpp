// Unified slash-command dispatcher for every platform.
//
// In Python, COMMAND_REGISTRY is defined once in hermes_cli/commands.py
// and the gateway wires each command to handlers.  The C++ port mirrors
// that: commands are registered once with a handler, and the dispatcher
// resolves a raw message text into a ``DispatchResult`` telling the
// runner what to do.
//
// The dispatcher is platform-agnostic — it receives the session key,
// platform, and the raw text; the handler decides how to respond.
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <hermes/gateway/gateway_runner.hpp>  // MessageEvent, Platform

namespace hermes::gateway {

// How the dispatcher wants the runner to react to a resolved command.
enum class CommandOutcome {
    // Unknown / non-command — runner should dispatch to the agent.
    NotCommand,
    // Recognized and handled in-process; runner should not dispatch.
    Handled,
    // Recognized, but the runner must take a follow-up action (reset
    // agent, drain queue, etc.) — see ``action``.
    HandledRequiresRunnerAction,
};

enum class CommandAction {
    None,
    StopAgent,
    ResetSession,
    DrainPending,
    ReplayPending,
    SwitchModel,
    RestartGateway,
    ShutdownGateway,
    ApproveToolCall,
    DenyToolCall,
};

struct DispatchResult {
    CommandOutcome outcome = CommandOutcome::NotCommand;
    CommandAction action = CommandAction::None;

    // Canonical command name (lowercased, no slash).
    std::string command;

    // Everything after the command word (trimmed).
    std::string args;

    // Human-readable response to send back to the user (optional).
    std::string reply;
};

struct CommandContext {
    std::string session_key;
    Platform platform = Platform::Local;
    const MessageEvent* event = nullptr;
};

class CommandDispatcher {
public:
    using Handler = std::function<DispatchResult(const CommandContext&,
                                                    const std::string& args)>;

    CommandDispatcher();

    // Register a command.  ``name`` is the primary command word without
    // the leading slash.  ``aliases`` is an optional list of alternate
    // names.  Re-registration overwrites.
    void register_command(std::string name, Handler handler,
                            std::vector<std::string> aliases = {});

    // Batch-register every gateway built-in: /stop, /reset, /new,
    // /approve, /deny, /model, /restart, /shutdown, /help.
    void register_builtins();

    // Strip command, lookup handler, invoke.  Returns NotCommand when
    // the text is not a slash command or the command is unknown.
    DispatchResult dispatch(const CommandContext& ctx,
                             const std::string& text);

    // True if ``name`` (without slash) maps to a registered handler.
    bool has_command(const std::string& name) const;

    // Introspection.
    std::vector<std::string> command_names() const;
    std::vector<std::string> aliases_of(const std::string& name) const;

    // Bypass — useful when the runner wants to pre-check whether a
    // message would trigger a command before doing heavier work.
    std::optional<std::string> resolve(const std::string& text) const;

    // Drop all handlers.  Used by tests.
    void clear();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Handler> handlers_;
    std::unordered_map<std::string, std::string> alias_of_;    // alias -> canonical
    std::unordered_map<std::string, std::vector<std::string>>
        aliases_for_;                                          // canonical -> aliases
};

}  // namespace hermes::gateway
