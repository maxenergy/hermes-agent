#include "hermes/approval/session_state.hpp"

#include <regex>
#include <utility>

namespace hermes::approval {

namespace {

thread_local std::string g_current_session_key;

}  // namespace

void SessionApprovalState::set_current_session_key(std::string key) {
    g_current_session_key = std::move(key);
}

std::string SessionApprovalState::current_session_key() {
    return g_current_session_key;
}

void SessionApprovalState::add_permanent_approval(
    const std::string& pattern_regex) {
    std::lock_guard<std::mutex> lock(mu_);
    permanent_.push_back(pattern_regex);
}

void SessionApprovalState::clear_permanent_approvals() {
    std::lock_guard<std::mutex> lock(mu_);
    permanent_.clear();
}

bool SessionApprovalState::is_permanently_approved(
    const std::string& command) const {
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot = permanent_;
    }
    for (const auto& pat : snapshot) {
        try {
            std::regex rx(pat, std::regex::ECMAScript | std::regex::icase);
            if (std::regex_search(command, rx)) {
                return true;
            }
        } catch (const std::regex_error&) {
            // Bad permanent regex — skip; never crashes the scanner.
        }
    }
    return false;
}

void SessionApprovalState::approve_pattern_for_session(
    const std::string& session_key, const std::string& pattern_key) {
    std::lock_guard<std::mutex> lock(mu_);
    session_approved_[session_key].insert(pattern_key);
}

void SessionApprovalState::approve_all_for_session(
    const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    yolo_sessions_.insert(session_key);
}

void SessionApprovalState::clear_session(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    session_approved_.erase(session_key);
    yolo_sessions_.erase(session_key);
}

bool SessionApprovalState::is_approved(const std::string& session_key,
                                       const std::string& pattern_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (yolo_sessions_.count(session_key)) return true;
    auto it = session_approved_.find(session_key);
    if (it == session_approved_.end()) return false;
    return it->second.count(pattern_key) > 0;
}

bool SessionApprovalState::is_yolo(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return yolo_sessions_.count(session_key) > 0;
}

ApprovalDecision SessionApprovalState::request_cli_approval(
    const std::string& session_key, const std::string& command,
    const std::vector<Match>& matches,
    const UserPromptCallback& user_prompt_cb) {
    // Pass-through: nothing dangerous matched.
    if (matches.empty()) {
        return ApprovalDecision::Approved;
    }

    // Yolo wins.
    if (is_yolo(session_key)) {
        return ApprovalDecision::Approved;
    }

    // Permanent allowlist (regex matched against the raw command).
    if (is_permanently_approved(command)) {
        return ApprovalDecision::Approved;
    }

    // All matched patterns already approved this session?
    bool all_approved = true;
    for (const auto& m : matches) {
        if (!is_approved(session_key, m.pattern_key)) {
            all_approved = false;
            break;
        }
    }
    if (all_approved) {
        return ApprovalDecision::Approved;
    }

    // Need user input.
    if (!user_prompt_cb) {
        return ApprovalDecision::Denied;
    }

    ApprovalRequest req;
    req.request_id = "cli";  // CLI flow doesn't need uuid; gateway does
    req.session_key = session_key;
    req.command = command;
    req.matches = matches;
    req.requested_at = std::chrono::system_clock::now();

    ApprovalDecision decision = user_prompt_cb(req);

    switch (decision) {
        case ApprovalDecision::Approved:
            for (const auto& m : matches) {
                approve_pattern_for_session(session_key, m.pattern_key);
            }
            return ApprovalDecision::Approved;
        case ApprovalDecision::Yolo:
            approve_all_for_session(session_key);
            return ApprovalDecision::Approved;
        case ApprovalDecision::Denied:
        case ApprovalDecision::Pending:
        default:
            return ApprovalDecision::Denied;
    }
}

}  // namespace hermes::approval
