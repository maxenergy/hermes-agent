// C++17 port of the pure-logic helpers from `hermes_cli/webhook.py`.
//
// The `hermes webhook` subcommand orchestrates filesystem mutation
// (`webhook_subscriptions.json`), config loading, and HTTP delivery.
// This port exposes the deterministic building blocks that can be
// unit-tested in isolation:
//
//   * `normalize_subscription_name` -- trim + lowercase + spaces-to-
//     hyphens, matching the first line of `_cmd_subscribe`.
//   * `is_valid_subscription_name` -- validate against the regex
//     `^[a-z0-9][a-z0-9_-]*$`.
//   * `parse_csv_field` -- split a `"a, b, c"` argument into a trimmed
//     vector, matching the one-liner used for `args.events` and
//     `args.skills`.
//   * `build_webhook_base_url` -- render `http://<host>:<port>`
//     honouring the `0.0.0.0 -> localhost` aliasing.
//   * `build_subscription_url` -- `<base>/webhooks/<name>`.
//   * `build_subscription_record` -- construct the JSON object saved
//     to `webhook_subscriptions.json` for a subscribe request.
//   * `compute_hmac_sha256_signature` -- "sha256=<hex>" signature for
//     the `X-Hub-Signature-256` header used by `hermes webhook test`.
//     The actual HTTP POST and HMAC primitives stay in the caller.
//   * `format_list_entry` / `format_subscribe_summary` -- plain-text
//     output lines, matching the Python `print()` sequences.
//   * `truncate_prompt_preview` -- 80-character preview with trailing
//     `"..."`.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hermes::cli::webhook_cmd {

// Normalise `args.name` -> trim, lowercase, spaces -> hyphens.
std::string normalize_subscription_name(const std::string& raw);

// Validate a subscription name against the regex used by the Python
// implementation (`^[a-z0-9][a-z0-9_-]*$`).
bool is_valid_subscription_name(const std::string& name);

// Split `"a, b, c"` into `{"a", "b", "c"}` -- empty input yields an
// empty vector (NOT `{""}`) matching the Python `[] if not args.events`
// branch.
std::vector<std::string> parse_csv_field(const std::string& csv);

// Render `http://<host>:<port>` with the `0.0.0.0 -> localhost` alias.
std::string build_webhook_base_url(const std::string& host, int port);

// `<base>/webhooks/<name>`.
std::string build_subscription_url(const std::string& base_url,
                                   const std::string& name);

// Input for `build_subscription_record`.  Mirrors argparse attrs.
struct subscribe_inputs {
    std::string name{};
    std::string description{};
    std::string events_csv{};
    std::string secret{};
    std::string prompt{};
    std::string skills_csv{};
    std::string deliver{};
    std::string deliver_chat_id{};
    std::string created_at_iso{};  // pre-formatted UTC timestamp
};

// Build the JSON object persisted into `webhook_subscriptions.json`.
// A missing `secret` in inputs keeps the caller responsible for
// generating a random token; this helper does not invent one.
nlohmann::json build_subscription_record(const subscribe_inputs& inputs);

// Compute the HMAC-SHA256 digest of `payload` with `secret`, rendered
// as `"sha256=<hex>"` for the webhook-test header.  `hmac_hex` is the
// raw hex digest supplied by the caller (so tests can drive the
// formatter without pulling in OpenSSL here).
std::string format_signature_header(const std::string& hmac_hex);

// Truncate `prompt` to 80 characters with trailing ellipsis.
std::string truncate_prompt_preview(const std::string& prompt);

// Render one `hermes webhook list` entry as a sequence of lines.
std::vector<std::string> format_list_entry(const std::string& name,
                                           const nlohmann::json& route,
                                           const std::string& base_url);

// Render the "Created/Updated webhook subscription..." block printed
// by `_cmd_subscribe`.  `is_update` distinguishes the "Created" vs
// "Updated" header.
std::vector<std::string> format_subscribe_summary(
    const std::string& name,
    const nlohmann::json& route,
    const std::string& base_url,
    bool is_update);

// Shared setup-hint text body shown when the webhook platform is
// disabled.  `hermes_home_display` is the user-facing path (e.g.
// `~/.hermes`).
std::string webhook_setup_hint(const std::string& hermes_home_display);

}  // namespace hermes::cli::webhook_cmd
