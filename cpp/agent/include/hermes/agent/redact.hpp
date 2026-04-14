// Regex-based secret redaction for logs and tool output.
//
// C++17 port of agent/redact.py. Applies pattern matching to mask API keys,
// tokens, and credentials before they reach log files, verbose output, or
// gateway logs.
//
// Short tokens (< 18 chars) are fully masked ("***"). Longer tokens preserve
// the first 6 and last 4 characters for debuggability.
#pragma once

#include <string>

namespace hermes::agent::redact {

// Returns true if HERMES_REDACT_SECRETS env var is unset or truthy (default on).
// Snapshot at first call so runtime env mutations cannot disable redaction
// mid-session.
bool is_enabled();

// Force-reset the cached enablement (for tests). Not thread-safe.
void reset_enabled_cache_for_testing();

// Mask a token, preserving a prefix/suffix for tokens >= 18 chars.
std::string mask_token(const std::string& token);

// Apply all redaction patterns to a block of text. Safe to call on any
// string; non-matching text passes through unchanged. No-op when disabled.
std::string redact_sensitive_text(const std::string& text);

}  // namespace hermes::agent::redact
