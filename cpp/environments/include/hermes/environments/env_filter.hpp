// env_filter — filter sensitive environment variables before passing
// them to sandboxed or remote environments.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace hermes::environments {

// Returns true when `name` is a known-sensitive environment variable.
//
// Match criteria (case-insensitive suffix):
//   _API_KEY, _TOKEN, _SECRET, _PASSWORD, _CLIENT_SECRET
//
// Explicit blocklist (exact match, case-sensitive):
//   ANTHROPIC_API_KEY, OPENAI_API_KEY, OPENROUTER_API_KEY,
//   TELEGRAM_BOT_TOKEN, DISCORD_BOT_TOKEN, SLACK_BOT_TOKEN,
//   AWS_SECRET_ACCESS_KEY, GITHUB_TOKEN, HERMES_API_KEY,
//   DATABASE_URL, REDIS_URL
//
// HERMES_HOME is intentionally **not** sensitive — it holds the
// installation root and is needed by child processes to locate
// configuration and optional-skills directories.
bool is_sensitive_var(std::string_view name);

// Returns a copy of `input` with all sensitive vars removed.
std::unordered_map<std::string, std::string> filter_env(
    const std::unordered_map<std::string, std::string>& input);

}  // namespace hermes::environments
