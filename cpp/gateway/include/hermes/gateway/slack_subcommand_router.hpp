// Phase 12 — Slack `/hermes` subcommand router.
//
// Slack slash commands receive raw text after the trigger.  This router
// parses the leading word into a canonical command name (mirrors
// hermes::cli::slack_subcommand_map) and dispatches to a registered
// handler.  Mirrors the Python gateway's slack_subcommand_map() helper.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hermes::gateway {

struct SlackSubcommand {
    std::string name;          // canonical command, e.g. "new"
    std::string remainder;     // text after the command word (trimmed)
    std::vector<std::string> argv;  // whitespace-split remainder
};

// Parse the body of a Slack slash command.  An empty / whitespace-only
// text yields nullopt.  Handles a leading slash on the command word
// (e.g. "/new" → "new").  Aliases must be resolved by the caller.
std::optional<SlackSubcommand> parse_slack_subcommand(const std::string& text);

class SlackSubcommandRouter {
public:
    using Handler = std::function<std::string(const SlackSubcommand&)>;

    // Register a handler for a canonical command name. Overwrites a prior
    // registration for the same name.
    void register_handler(const std::string& name, Handler h);

    // Register a list of canonical commands derived from
    // hermes::cli::slack_subcommand_map() so that unknown subcommands
    // can be reported with a friendly error.
    void set_known_commands(const std::map<std::string, std::string>& commands);

    // Dispatch a raw command body. Returns the handler's response string
    // (suitable for posting back to Slack).  When no handler matches,
    // returns a hint listing known commands.  When the body is empty,
    // returns the help text.
    std::string dispatch(const std::string& text) const;

    // Test introspection.
    bool has_handler(const std::string& name) const;

private:
    std::map<std::string, Handler> handlers_;
    std::map<std::string, std::string> known_;
};

}  // namespace hermes::gateway
