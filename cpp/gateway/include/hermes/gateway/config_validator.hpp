// Gateway config schema validation + default injection + migrations.
//
// Mirrors gateway/config.py's DEFAULT_CONFIG / migration hooks.  The
// loader in gateway_config.cpp performs the structural parse; the
// validator here enforces invariants and documents what fields each
// platform block must carry.
//
//   - validate_gateway_config   — return a list of diagnostics
//   - inject_defaults           — fill missing fields from DEFAULT_CONFIG
//   - migrate_gateway_config    — walk the config_version ladder
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/gateway/gateway_config.hpp>

namespace hermes::gateway {

// Current config schema version.  Bumped whenever a breaking layout
// change lands (see gateway/config.py::_config_version).
inline constexpr int kCurrentConfigVersion = 7;

enum class ValidationSeverity {
    Info,
    Warning,
    Error,
};

struct ValidationDiagnostic {
    ValidationSeverity severity = ValidationSeverity::Info;
    std::string path;    // dotted JSON pointer e.g. "platforms.telegram.token"
    std::string message;
};

struct ValidationReport {
    std::vector<ValidationDiagnostic> diagnostics;
    bool ok = true;        // false when at least one Error exists

    std::size_t count(ValidationSeverity sev) const;
    std::vector<ValidationDiagnostic> filter(ValidationSeverity sev) const;
    std::string to_string() const;
};

// Validate a parsed ``config.yaml`` JSON blob.  Returns a report; use
// ``report.ok`` to gate startup.
ValidationReport validate_gateway_config(const nlohmann::json& config);

// Apply built-in defaults to any missing keys (in-place).  Returns the
// number of fields injected.
std::size_t inject_defaults(nlohmann::json& config);

// Migrate ``config`` from its current ``config_version`` to
// ``kCurrentConfigVersion``.  Steps are chained — v3 → v4 → v5 → …
// Returns the previous version (or -1 when no version field exists).
int migrate_gateway_config(nlohmann::json& config);

// Per-platform field expectations.  Each entry defines the required
// keys and their accepted types.  Used by ``validate_gateway_config``.
struct PlatformSchema {
    Platform platform;
    std::vector<std::string> required_fields;   // absolute keys
    std::vector<std::string> optional_fields;
    std::vector<std::string> secret_fields;     // warn if logged
};

// Registered schemas.  Sorted by platform enum.
const std::vector<PlatformSchema>& platform_schemas();

// Look up the schema for a specific platform (nullptr if none).
const PlatformSchema* schema_for(Platform platform);

// Convenience helper used by tests.  Returns true if ``value`` is a
// plausible non-empty secret (long enough, no whitespace, not a
// placeholder like ``YOUR_TOKEN_HERE``).
bool is_plausible_secret(std::string_view value);

// Merge two gateway config blobs — ``overrides`` wins.  Arrays are
// replaced, not appended.
nlohmann::json merge_gateway_configs(const nlohmann::json& base,
                                      const nlohmann::json& overrides);

// Strip known secret-bearing fields from ``config`` so it can be safely
// logged.  Returns a new blob; ``config`` is untouched.
nlohmann::json redact_secrets(const nlohmann::json& config);

}  // namespace hermes::gateway
