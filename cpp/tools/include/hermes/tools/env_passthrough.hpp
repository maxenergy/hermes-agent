// Environment-variable filter for sandbox/subprocess passthrough.
//
// Strips known-secret variables from the inherited environment so child
// processes (terminal tool, code-execution sandbox, remote backends)
// don't accidentally see API keys.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace hermes::tools {

// Snapshot of the current environment with sensitive entries removed.
std::unordered_map<std::string, std::string> safe_env_subset();

// Return true when ``name`` looks like a credential or hermes-internal var
// that should never be passed through.  Matches:
//   * *_API_KEY
//   * *_BOT_TOKEN, *_TOKEN
//   * HERMES_*
//   * a hand-curated allow-list of known providers (OPENAI, ANTHROPIC,
//     OPENROUTER, ELEVENLABS, ...).
bool is_sensitive_env_var(std::string_view name);

}  // namespace hermes::tools
