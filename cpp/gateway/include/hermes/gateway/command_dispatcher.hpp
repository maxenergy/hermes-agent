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

// Routing classification for an incoming slash command when a session
// is already running an agent turn.  Upstream 632a807a redesigned this
// from an allow-list to "any recognized slash command bypasses".
enum class ActiveSessionRouting {
    // Not a slash command — caller dispatches to the agent (may queue
    // if busy).
    NotACommand,
    // Recognized slash command with a dedicated running-agent handler
    // (e.g. /stop, /help, /status, /agents).  Invoke the handler
    // directly without touching the running agent.
    BypassRunsDedicatedHandler,
    // Recognized slash command without a dedicated handler.  Reject
    // with a user-visible "agent busy — wait or /stop" reply; never
    // interrupt the agent.
    BypassGracefulReject,
    // /queue <prompt> — bypasses the active-session guard entirely,
    // queues the prompt for the next turn, does not interrupt.
    QueueBypass,
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
    // names.  ``has_dedicated_running_handler`` marks commands that
    // have a Level-2 handler capable of running while an agent turn
    // is in-flight (e.g. /stop, /status, /help — upstream's
    // ACTIVE_SESSION_BYPASS_COMMANDS).  Re-registration overwrites.
    void register_command(std::string name, Handler handler,
                            std::vector<std::string> aliases = {},
                            bool has_dedicated_running_handler = false);

    // True if ``name`` was registered with ``has_dedicated_running_handler``.
    bool has_dedicated_running_handler(const std::string& name) const;

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

    // Classify an inbound event that arrived while a session's agent is
    // running.  Implements upstream 632a807a: every recognized slash
    // command bypasses the active-session interrupt path.  Commands
    // with a dedicated running-agent handler get
    // ``BypassRunsDedicatedHandler``; commands that lack one get
    // ``BypassGracefulReject`` with a canned "wait or /stop" reply.
    // ``/queue <prompt>`` explicitly bypasses both.  Plain text or
    // unrecognized slash tokens return ``NotACommand`` — the runner
    // handles them via the normal busy-session queue path.
    struct ActiveSessionDecision {
        ActiveSessionRouting routing = ActiveSessionRouting::NotACommand;
        std::string canonical;   // Resolved command name, if any.
        std::string reply;       // For BypassGracefulReject.
    };
    ActiveSessionDecision classify_for_active_session(
        const std::string& text) const;

    // Drop all handlers.  Used by tests.
    void clear();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Handler> handlers_;
    std::unordered_map<std::string, std::string> alias_of_;    // alias -> canonical
    std::unordered_map<std::string, std::vector<std::string>>
        aliases_for_;                                          // canonical -> aliases
    std::unordered_map<std::string, bool> dedicated_handlers_; // canonical -> bool
};

}  // namespace hermes::gateway
