// MCP credential stripping for error messages.
//
// Layers on top of hermes::core::redact::redact_secrets (Phase 0) by
// running the core redactor first, then applying additional MCP-specific
// patterns: Bearer tokens, OpenAI sk-*, GitHub ghp_*/gho_*, AWS AKIA*,
// SSH ssh-rsa AAAA*, and the common KEY=VALUE forms (API_KEY, OPENAI_API_KEY,
// password, secret, token, key, x-api-key, private_key "...").
//
// Used by the MCP tool wrapper to sanitize subprocess error output before
// it reaches the LLM or logs.
#pragma once

#include <string>
#include <string_view>

namespace hermes::approval {

std::string strip_credentials(std::string_view input);

}  // namespace hermes::approval
