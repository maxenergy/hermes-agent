// SessionApprovalState — per-session approval state machinery shared by
// the CLI and gateway frontends.
//
//   * permanent allowlist (loaded from config.yaml)
//   * per-session approved pattern keys
//   * per-session yolo bypass
//   * thread_local current session key (so nested subagents inherit
//     the parent's session unless they enter their own context)
//   * blocking CLI flow that consults a user-supplied prompt callback
//
// All methods are thread-safe.
#pragma once

#include "hermes/approval/command_scanner.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::approval {

enum class ApprovalDecision { Pending, Approved, Denied, Yolo };

struct ApprovalRequest {
    std::string request_id;
    std::string session_key;
    std::string command;
    std::vector<Match> matches;
    std::chrono::system_clock::time_point requested_at;
};

class SessionApprovalState {
public:
    SessionApprovalState() = default;

    // Thread-local session key context. Subagents that don't override
    // this inherit the parent's value.
    static void set_current_session_key(std::string key);
    static std::string current_session_key();

    // Permanent allowlist (regex strings, matched against full command).
    void add_permanent_approval(const std::string& pattern_regex);
    void clear_permanent_approvals();
    bool is_permanently_approved(const std::string& command) const;

    // Per-session runtime approval set keyed by danger pattern key.
    void approve_pattern_for_session(const std::string& session_key,
                                     const std::string& pattern_key);
    void approve_all_for_session(const std::string& session_key);  // yolo
    void clear_session(const std::string& session_key);

    bool is_approved(const std::string& session_key,
                     const std::string& pattern_key) const;
    bool is_yolo(const std::string& session_key) const;

    // CLI blocking flow. Steps:
    //   1. matches empty           → Approved
    //   2. yolo session            → Approved
    //   3. permanent allowlist hit → Approved
    //   4. all matches already approved for session → Approved
    //   5. otherwise call user_prompt_cb(req); remember the result
    using UserPromptCallback =
        std::function<ApprovalDecision(const ApprovalRequest& req)>;

    ApprovalDecision request_cli_approval(const std::string& session_key,
                                          const std::string& command,
                                          const std::vector<Match>& matches,
                                          const UserPromptCallback& user_prompt_cb);

private:
    mutable std::mutex mu_;
    std::vector<std::string> permanent_;  // raw regex strings
    std::unordered_map<std::string, std::unordered_set<std::string>>
        session_approved_;
    std::unordered_set<std::string> yolo_sessions_;
};

}  // namespace hermes::approval
