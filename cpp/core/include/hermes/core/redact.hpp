// Redact API keys and bearer tokens from arbitrary text.
#pragma once

#include <string>
#include <string_view>

namespace hermes::core::redact {

// Replace patterns that look like credentials (OpenAI sk-*, GitHub
// ghp_*, Bearer tokens, `token=` / `key=` / `password=` / `secret=`
// query parameters) with `***REDACTED***`.
std::string redact_secrets(std::string_view input);

}  // namespace hermes::core::redact
