// C++17 port of the pure-logic helpers from `hermes_cli/copilot_auth.py`.

#include "hermes/cli/copilot_auth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hermes::cli::copilot_auth {
namespace {

std::string trim(const std::string& value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string{first, last};
}

bool starts_with(const std::string& value, const std::string& prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return value.compare(0, prefix.size(), prefix) == 0;
}

std::string default_env_lookup(const char* name) {
    if (name == nullptr) {
        return std::string{};
    }
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::string{};
    }
    return std::string{value};
}

}  // namespace

token_validation_result validate_copilot_token(const std::string& raw_token) {
    std::string token{trim(raw_token)};
    if (token.empty()) {
        return token_validation_result{false, std::string{"Empty token"}};
    }

    if (starts_with(token, std::string{k_classic_pat_prefix})) {
        std::ostringstream oss{};
        oss << "Classic Personal Access Tokens (ghp_*) are not supported by the "
            << "Copilot API. Use one of:\n"
            << "  \xe2\x86\x92 `copilot login` or `hermes model` to authenticate via OAuth\n"
            << "  \xe2\x86\x92 A fine-grained PAT (github_pat_*) with Copilot Requests permission\n"
            << "  \xe2\x86\x92 `gh auth login` with the default device code flow (produces gho_* tokens)";
        return token_validation_result{false, oss.str()};
    }

    return token_validation_result{true, std::string{"OK"}};
}

token_resolution resolve_copilot_token_from_env(env_lookup_fn lookup) {
    env_lookup_fn fn = lookup != nullptr ? lookup : &default_env_lookup;

    for (const char* env_var : k_env_vars) {
        std::string raw{fn(env_var)};
        std::string value{trim(raw)};
        if (value.empty()) {
            continue;
        }
        token_validation_result res{validate_copilot_token(value)};
        if (!res.valid) {
            continue;
        }
        return token_resolution{std::move(value), std::string{env_var}};
    }

    return token_resolution{};
}

std::vector<std::string> gh_cli_candidates(
    const std::string& resolved_path,
    const std::string& home_dir,
    executable_check_fn executable_check) {
    std::vector<std::string> candidates{};
    std::unordered_set<std::string> seen{};

    auto push_unique = [&](std::string path) {
        if (path.empty()) {
            return;
        }
        if (seen.count(path) > 0) {
            return;
        }
        seen.insert(path);
        candidates.push_back(std::move(path));
    };

    if (!resolved_path.empty()) {
        push_unique(resolved_path);
    }

    std::vector<std::string> static_paths{
        std::string{"/opt/homebrew/bin/gh"},
        std::string{"/usr/local/bin/gh"},
    };
    if (!home_dir.empty()) {
        static_paths.push_back(home_dir + std::string{"/.local/bin/gh"});
    }

    for (const auto& candidate : static_paths) {
        if (seen.count(candidate) > 0) {
            continue;
        }
        if (executable_check != nullptr && !executable_check(candidate)) {
            continue;
        }
        if (executable_check == nullptr) {
            continue;
        }
        push_unique(candidate);
    }

    return candidates;
}

std::unordered_map<std::string, std::string> copilot_request_headers(
    bool is_agent_turn,
    bool is_vision) {
    std::unordered_map<std::string, std::string> headers{};
    headers.emplace("Editor-Version", std::string{"vscode/1.104.1"});
    headers.emplace("User-Agent", std::string{"HermesAgent/1.0"});
    headers.emplace("Copilot-Integration-Id", std::string{"vscode-chat"});
    headers.emplace("Openai-Intent", std::string{"conversation-edits"});
    headers.emplace("x-initiator",
                    std::string{is_agent_turn ? "agent" : "user"});
    if (is_vision) {
        headers.emplace("Copilot-Vision-Request", std::string{"true"});
    }
    return headers;
}

device_code_poll_decision classify_device_code_response(
    const std::unordered_map<std::string, std::string>& response) {
    device_code_poll_decision decision{};

    auto access_it = response.find("access_token");
    if (access_it != response.end() && !access_it->second.empty()) {
        decision.status = device_code_status::success;
        decision.access_token = access_it->second;
        return decision;
    }

    auto error_it = response.find("error");
    std::string error{error_it != response.end() ? error_it->second : std::string{}};

    if (error == "authorization_pending") {
        decision.status = device_code_status::pending;
        return decision;
    }
    if (error == "slow_down") {
        decision.status = device_code_status::slow_down;
        return decision;
    }
    if (error == "expired_token") {
        decision.status = device_code_status::expired;
        decision.error_message = std::string{"Device code expired. Please try again."};
        return decision;
    }
    if (error == "access_denied") {
        decision.status = device_code_status::access_denied;
        decision.error_message = std::string{"Authorization was denied."};
        return decision;
    }

    decision.status = device_code_status::error;
    decision.error_message = error.empty()
        ? std::string{"Unknown authorization failure"}
        : std::string{"Authorization failed: "} + error;
    return decision;
}

int compute_next_poll_interval(int current_interval,
                               const std::string& server_interval_str) {
    std::string trimmed{trim(server_interval_str)};
    if (!trimmed.empty()) {
        try {
            std::size_t consumed{0};
            double parsed{std::stod(trimmed, &consumed)};
            if (consumed == trimmed.size() && parsed > 0.0) {
                return static_cast<int>(parsed);
            }
        } catch (const std::exception&) {
            // Fall through to the additive bump below.
        }
    }
    return current_interval + 5;
}

}  // namespace hermes::cli::copilot_auth
