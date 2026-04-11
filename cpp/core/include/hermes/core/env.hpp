// Environment-variable helpers and a minimal dotenv loader.
#pragma once

#include <filesystem>
#include <string_view>

namespace hermes::core::env {

// True when the named env var is set AND its value parses as truthy
// (`1`, `true`, `yes`, `on` — case-insensitive).
bool env_var_enabled(std::string_view name);

// Same truthiness test without the env lookup.
bool is_truthy_value(std::string_view value);

// Parse KEY=VALUE lines from a dotenv-style file and inject them into
// the process environment via ::setenv(). Supports `#` line comments,
// single/double quoted values, and `${VAR}` expansion. Missing files
// are silently ignored.
void load_dotenv(const std::filesystem::path& path);

}  // namespace hermes::core::env
