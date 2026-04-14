#include <hermes/gateway/command_dispatcher.hpp>

#include <algorithm>
#include <cctype>

#include <hermes/gateway/gateway_helpers.hpp>

namespace hermes::gateway {

namespace {

std::string lowercase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    return out;
}

}  // namespace

CommandDispatcher::CommandDispatcher() = default;

void CommandDispatcher::register_command(std::string name, Handler handler,
                                            std::vector<std::string> aliases) {
    auto lname = lowercase(name);
    std::lock_guard<std::mutex> lock(mu_);
    handlers_[lname] = std::move(handler);
    for (auto& a : aliases) {
        auto la = lowercase(a);
        alias_of_[la] = lname;
        aliases_for_[lname].push_back(la);
    }
}

void CommandDispatcher::register_builtins() {
    auto handler = [](CommandAction action, std::string reply) {
        return [action, reply = std::move(reply)](
                    const CommandContext&,
                    const std::string&) -> DispatchResult {
            DispatchResult r;
            r.outcome = CommandOutcome::HandledRequiresRunnerAction;
            r.action = action;
            r.reply = reply;
            return r;
        };
    };

    register_command("stop", handler(CommandAction::StopAgent,
                                       "Stopping the current turn..."),
                     {"cancel", "abort"});
    register_command("reset", handler(CommandAction::ResetSession,
                                        "Conversation reset."),
                     {"new"});
    register_command("drain", handler(CommandAction::DrainPending,
                                        "Pending queue cleared."),
                     {"clear"});
    register_command("replay", handler(CommandAction::ReplayPending,
                                         "Replaying queued messages..."));
    register_command("model",
                     [](const CommandContext&,
                         const std::string& args) -> DispatchResult {
                         DispatchResult r;
                         r.outcome = CommandOutcome::HandledRequiresRunnerAction;
                         r.action = CommandAction::SwitchModel;
                         r.args = args;
                         r.reply = args.empty()
                                        ? std::string("No model supplied.")
                                        : ("Switching model to " + args);
                         return r;
                     });
    register_command("restart", handler(CommandAction::RestartGateway,
                                          "Gateway restarting..."));
    register_command("shutdown", handler(CommandAction::ShutdownGateway,
                                           "Gateway shutting down..."),
                     {"quit", "exit"});
    register_command("approve", handler(CommandAction::ApproveToolCall,
                                          "Approval acknowledged."),
                     {"yes", "ok"});
    register_command("deny", handler(CommandAction::DenyToolCall,
                                        "Tool call denied."),
                     {"no", "reject"});
    register_command("help",
                     [this](const CommandContext&,
                             const std::string&) -> DispatchResult {
                         DispatchResult r;
                         r.outcome = CommandOutcome::Handled;
                         std::string lines = "Available commands:\n";
                         for (auto& n : command_names()) {
                             lines += "  /" + n + "\n";
                         }
                         r.reply = lines;
                         return r;
                     });
}

DispatchResult CommandDispatcher::dispatch(const CommandContext& ctx,
                                              const std::string& text) {
    auto word = extract_command_word(text);
    if (!word) return {};
    auto args = extract_command_args(text);

    std::unique_lock<std::mutex> lock(mu_);
    auto lword = *word;
    auto alias_it = alias_of_.find(lword);
    if (alias_it != alias_of_.end()) lword = alias_it->second;
    auto it = handlers_.find(lword);
    if (it == handlers_.end()) {
        DispatchResult r;
        r.outcome = CommandOutcome::NotCommand;
        r.command = lword;
        r.args = args;
        return r;
    }
    auto handler = it->second;
    lock.unlock();

    auto result = handler(ctx, args);
    if (result.command.empty()) result.command = lword;
    if (result.args.empty()) result.args = args;
    return result;
}

bool CommandDispatcher::has_command(const std::string& name) const {
    auto l = lowercase(name);
    std::lock_guard<std::mutex> lock(mu_);
    if (handlers_.count(l)) return true;
    return alias_of_.count(l) > 0;
}

std::vector<std::string> CommandDispatcher::command_names() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (auto& [k, _] : handlers_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> CommandDispatcher::aliases_of(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = aliases_for_.find(lowercase(name));
    if (it == aliases_for_.end()) return {};
    return it->second;
}

std::optional<std::string> CommandDispatcher::resolve(
    const std::string& text) const {
    auto word = extract_command_word(text);
    if (!word) return std::nullopt;
    auto lword = *word;
    std::lock_guard<std::mutex> lock(mu_);
    auto alias_it = alias_of_.find(lword);
    if (alias_it != alias_of_.end()) lword = alias_it->second;
    if (!handlers_.count(lword)) return std::nullopt;
    return lword;
}

void CommandDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    handlers_.clear();
    alias_of_.clear();
    aliases_for_.clear();
}

}  // namespace hermes::gateway
