// Tests for the C++17 port of `hermes_cli/copilot_auth.py`.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "hermes/cli/copilot_auth.hpp"

using namespace hermes::cli::copilot_auth;

namespace {

// Table-driven env lookup for deterministic tests.
const std::unordered_map<std::string, std::string>* g_env_table{nullptr};

std::string env_lookup_stub(const char* name) {
    if (g_env_table == nullptr || name == nullptr) {
        return std::string{};
    }
    auto it = g_env_table->find(std::string{name});
    if (it == g_env_table->end()) {
        return std::string{};
    }
    return it->second;
}

const std::unordered_set<std::string>* g_exec_table{nullptr};

bool exec_check_stub(const std::string& path) {
    if (g_exec_table == nullptr) {
        return false;
    }
    return g_exec_table->count(path) > 0;
}

}  // namespace

TEST(CopilotAuth, ValidateTokenRejectsEmpty) {
    auto res = validate_copilot_token("");
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.message, "Empty token");
}

TEST(CopilotAuth, ValidateTokenRejectsWhitespace) {
    auto res = validate_copilot_token("   \t\n");
    EXPECT_FALSE(res.valid);
    EXPECT_EQ(res.message, "Empty token");
}

TEST(CopilotAuth, ValidateTokenRejectsClassicPat) {
    auto res = validate_copilot_token("ghp_abc123");
    EXPECT_FALSE(res.valid);
    EXPECT_NE(res.message.find("Classic Personal Access Tokens"),
              std::string::npos);
}

TEST(CopilotAuth, ValidateTokenAcceptsOAuth) {
    auto res = validate_copilot_token("gho_abc");
    EXPECT_TRUE(res.valid);
    EXPECT_EQ(res.message, "OK");
}

TEST(CopilotAuth, ValidateTokenAcceptsFineGrainedPAT) {
    auto res = validate_copilot_token("github_pat_xxx");
    EXPECT_TRUE(res.valid);
}

TEST(CopilotAuth, ValidateTokenAcceptsAppToken) {
    auto res = validate_copilot_token("ghu_xxx");
    EXPECT_TRUE(res.valid);
}

TEST(CopilotAuth, ValidateTokenTrimsInput) {
    auto res = validate_copilot_token("  gho_trimmed  \n");
    EXPECT_TRUE(res.valid);
}

TEST(CopilotAuth, ResolveFromEnvPrefersCopilotVar) {
    std::unordered_map<std::string, std::string> env{
        {"COPILOT_GITHUB_TOKEN", "gho_primary"},
        {"GH_TOKEN", "gho_secondary"},
        {"GITHUB_TOKEN", "gho_tertiary"},
    };
    g_env_table = &env;
    auto res = resolve_copilot_token_from_env(&env_lookup_stub);
    g_env_table = nullptr;

    EXPECT_EQ(res.token, "gho_primary");
    EXPECT_EQ(res.source, "COPILOT_GITHUB_TOKEN");
}

TEST(CopilotAuth, ResolveFromEnvFallsThroughToGhToken) {
    std::unordered_map<std::string, std::string> env{
        {"GH_TOKEN", "gho_secondary"},
    };
    g_env_table = &env;
    auto res = resolve_copilot_token_from_env(&env_lookup_stub);
    g_env_table = nullptr;

    EXPECT_EQ(res.token, "gho_secondary");
    EXPECT_EQ(res.source, "GH_TOKEN");
}

TEST(CopilotAuth, ResolveFromEnvSkipsClassicPat) {
    std::unordered_map<std::string, std::string> env{
        {"COPILOT_GITHUB_TOKEN", "ghp_classic"},
        {"GITHUB_TOKEN", "github_pat_fine"},
    };
    g_env_table = &env;
    auto res = resolve_copilot_token_from_env(&env_lookup_stub);
    g_env_table = nullptr;

    EXPECT_EQ(res.token, "github_pat_fine");
    EXPECT_EQ(res.source, "GITHUB_TOKEN");
}

TEST(CopilotAuth, ResolveFromEnvEmptyWhenAllMissing) {
    std::unordered_map<std::string, std::string> env{};
    g_env_table = &env;
    auto res = resolve_copilot_token_from_env(&env_lookup_stub);
    g_env_table = nullptr;

    EXPECT_TRUE(res.token.empty());
    EXPECT_TRUE(res.source.empty());
}

TEST(CopilotAuth, ResolveFromEnvTreatsBlankAsUnset) {
    std::unordered_map<std::string, std::string> env{
        {"COPILOT_GITHUB_TOKEN", "   "},
        {"GH_TOKEN", "gho_real"},
    };
    g_env_table = &env;
    auto res = resolve_copilot_token_from_env(&env_lookup_stub);
    g_env_table = nullptr;

    EXPECT_EQ(res.token, "gho_real");
    EXPECT_EQ(res.source, "GH_TOKEN");
}

TEST(CopilotAuth, GhCandidatesPrefersResolvedPath) {
    std::unordered_set<std::string> existing{
        "/opt/homebrew/bin/gh",
        "/home/user/.local/bin/gh",
    };
    g_exec_table = &existing;
    auto list = gh_cli_candidates("/usr/bin/gh", "/home/user", &exec_check_stub);
    g_exec_table = nullptr;

    ASSERT_FALSE(list.empty());
    EXPECT_EQ(list[0], "/usr/bin/gh");
    EXPECT_NE(std::find(list.begin(), list.end(), "/opt/homebrew/bin/gh"),
              list.end());
    EXPECT_NE(std::find(list.begin(), list.end(), "/home/user/.local/bin/gh"),
              list.end());
}

TEST(CopilotAuth, GhCandidatesDeduplicates) {
    std::unordered_set<std::string> existing{"/opt/homebrew/bin/gh"};
    g_exec_table = &existing;
    auto list = gh_cli_candidates("/opt/homebrew/bin/gh", "/home/user",
                                  &exec_check_stub);
    g_exec_table = nullptr;

    // Exactly one entry because both the which() and static fallback
    // resolved to the same path.
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0], "/opt/homebrew/bin/gh");
}

TEST(CopilotAuth, GhCandidatesSkipsMissingFiles) {
    std::unordered_set<std::string> existing{};
    g_exec_table = &existing;
    auto list = gh_cli_candidates("", "/home/user", &exec_check_stub);
    g_exec_table = nullptr;

    EXPECT_TRUE(list.empty());
}

TEST(CopilotAuth, RequestHeadersAgentTurn) {
    auto headers = copilot_request_headers(true, false);
    EXPECT_EQ(headers.at("x-initiator"), "agent");
    EXPECT_EQ(headers.at("Editor-Version"), "vscode/1.104.1");
    EXPECT_EQ(headers.at("Copilot-Integration-Id"), "vscode-chat");
    EXPECT_EQ(headers.at("Openai-Intent"), "conversation-edits");
    EXPECT_EQ(headers.at("User-Agent"), "HermesAgent/1.0");
    EXPECT_EQ(headers.count("Copilot-Vision-Request"), 0u);
}

TEST(CopilotAuth, RequestHeadersUserTurn) {
    auto headers = copilot_request_headers(false, false);
    EXPECT_EQ(headers.at("x-initiator"), "user");
}

TEST(CopilotAuth, RequestHeadersVisionAddsHeader) {
    auto headers = copilot_request_headers(true, true);
    EXPECT_EQ(headers.at("Copilot-Vision-Request"), "true");
}

TEST(CopilotAuth, ClassifySuccess) {
    auto decision = classify_device_code_response({{"access_token", "gho_x"}});
    EXPECT_EQ(decision.status, device_code_status::success);
    EXPECT_EQ(decision.access_token, "gho_x");
}

TEST(CopilotAuth, ClassifyAuthorizationPending) {
    auto decision =
        classify_device_code_response({{"error", "authorization_pending"}});
    EXPECT_EQ(decision.status, device_code_status::pending);
}

TEST(CopilotAuth, ClassifySlowDown) {
    auto decision = classify_device_code_response({{"error", "slow_down"}});
    EXPECT_EQ(decision.status, device_code_status::slow_down);
}

TEST(CopilotAuth, ClassifyExpired) {
    auto decision = classify_device_code_response({{"error", "expired_token"}});
    EXPECT_EQ(decision.status, device_code_status::expired);
    EXPECT_NE(decision.error_message.find("expired"), std::string::npos);
}

TEST(CopilotAuth, ClassifyAccessDenied) {
    auto decision = classify_device_code_response({{"error", "access_denied"}});
    EXPECT_EQ(decision.status, device_code_status::access_denied);
    EXPECT_NE(decision.error_message.find("denied"), std::string::npos);
}

TEST(CopilotAuth, ClassifyOtherError) {
    auto decision =
        classify_device_code_response({{"error", "server_error"}});
    EXPECT_EQ(decision.status, device_code_status::error);
    EXPECT_NE(decision.error_message.find("server_error"), std::string::npos);
}

TEST(CopilotAuth, ClassifyEmptyErrorYieldsUnknown) {
    auto decision = classify_device_code_response({});
    EXPECT_EQ(decision.status, device_code_status::error);
    EXPECT_NE(decision.error_message.find("Unknown"), std::string::npos);
}

TEST(CopilotAuth, ClassifyPrefersAccessTokenOverError) {
    auto decision =
        classify_device_code_response({{"access_token", "tok"},
                                       {"error", "authorization_pending"}});
    EXPECT_EQ(decision.status, device_code_status::success);
    EXPECT_EQ(decision.access_token, "tok");
}

TEST(CopilotAuth, ComputeNextPollIntervalUsesServerValue) {
    EXPECT_EQ(compute_next_poll_interval(5, "10"), 10);
    EXPECT_EQ(compute_next_poll_interval(5, "7.4"), 7);
}

TEST(CopilotAuth, ComputeNextPollIntervalAddsFiveWhenMissing) {
    EXPECT_EQ(compute_next_poll_interval(5, ""), 10);
    EXPECT_EQ(compute_next_poll_interval(8, "  "), 13);
}

TEST(CopilotAuth, ComputeNextPollIntervalRejectsNonNumeric) {
    EXPECT_EQ(compute_next_poll_interval(5, "abc"), 10);
    EXPECT_EQ(compute_next_poll_interval(5, "0"), 10);
    EXPECT_EQ(compute_next_poll_interval(5, "-3"), 10);
}

TEST(CopilotAuth, ConstantsMatchPython) {
    EXPECT_STREQ(k_oauth_client_id, "Ov23li8tweQw6odWQebz");
    EXPECT_STREQ(k_classic_pat_prefix, "ghp_");
    EXPECT_EQ(k_device_code_poll_interval, 5);
    EXPECT_EQ(k_device_code_poll_safety_margin, 3);
    ASSERT_EQ(k_env_vars.size(), 3u);
    EXPECT_STREQ(k_env_vars[0], "COPILOT_GITHUB_TOKEN");
    EXPECT_STREQ(k_env_vars[1], "GH_TOKEN");
    EXPECT_STREQ(k_env_vars[2], "GITHUB_TOKEN");
    ASSERT_EQ(k_supported_prefixes.size(), 3u);
}
