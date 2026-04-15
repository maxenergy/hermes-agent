// Depth-port helpers for ``tools/browser_tool.py``.  Focused on the
// configuration layer — CDP endpoint resolution, cloud-provider
// registry lookup, allow_private_urls default, install-hint / error
// message composition, screenshot cleanup throttle, prompt assembly for
// snapshot extraction — that complement the URL-safety helpers already
// in browser_tool.hpp.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hermes::tools::browser_depth {

// ---- Command timeout --------------------------------------------------

// Minimum accepted browser command timeout.  Floored to avoid instant
// kills even when the user misconfigures.  Matches the Python ``max(int
// (val), 5)`` in ``_get_command_timeout``.
constexpr int kBrowserCommandTimeoutFloor = 5;
constexpr int kBrowserCommandTimeoutDefault = 30;

// Resolve a command_timeout config value into the final int used by the
// runtime.  Negative / zero / missing value returns the default.  All
// positive values are floored at ``kBrowserCommandTimeoutFloor``.
int resolve_command_timeout(std::optional<int> configured,
                            int default_value = kBrowserCommandTimeoutDefault);

// ---- CDP endpoint resolution ------------------------------------------

// Strategy for a supplied CDP URL — how ``_resolve_cdp_override`` should
// process it.
enum class CdpStrategy {
    // The URL already contains ``/devtools/browser/`` — use as-is.
    PassThroughWebsocket,
    // Bare ``ws://host:port`` / ``wss://host:port`` — probe via HTTP
    // discovery endpoint.
    WebsocketDiscovery,
    // Full ``http(s)://host:port`` or ``…/json/version`` — probe
    // ``/json/version`` for the real websocket URL.
    HttpDiscovery,
    // Empty / whitespace-only input.
    Empty,
};

// Classify the raw endpoint string.  Trims whitespace first.
CdpStrategy classify_cdp_endpoint(std::string_view raw);

// Compose the concrete ``/json/version`` discovery URL given a
// classified HTTP-discovery endpoint.  Does NOT perform HTTP IO.
std::string build_cdp_discovery_url(std::string_view http_or_ws_url);

// Pick the ``webSocketDebuggerUrl`` field from a discovery response
// body.  Returns the raw input URL when the payload is missing the
// field.  ``response_body`` is expected to be the JSON from
// ``/json/version``.
std::string pick_websocket_from_discovery(std::string_view raw_endpoint,
                                          std::string_view response_body);

// ---- Provider registry -------------------------------------------------

// Known cloud-browser provider names.  Matches ``_PROVIDER_REGISTRY``.
const std::vector<std::string>& supported_cloud_providers();

// Return ``true`` when ``name`` is a recognised cloud provider.  Case-
// sensitive — the caller should normalise via
// ``normalise_cloud_provider_name`` first.
bool is_known_cloud_provider(std::string_view name);

// Lower-case and trim a user-supplied cloud provider name.  Maps
// unknown values to an empty string.  Alias handling: ``browseruse`` →
// ``browser-use``, ``bb`` → ``browserbase``, ``none`` / ``off`` →
// ``local``.
std::string normalise_cloud_provider_name(std::string_view raw);

// ---- Install hints -----------------------------------------------------

// Return the ``npm install -g agent-browser …`` hint the tool emits
// when the binary is missing.  Branches on whether the environment is
// Termux (no ``--with-deps`` flag there).
std::string browser_install_hint(bool is_termux);

// Message emitted when the Termux environment falls back to ``npx
// agent-browser`` instead of a real install.
std::string termux_browser_install_error();

// Return ``true`` when the browser command string ``"npx agent-browser"``
// on Termux in local mode should trigger the install error.
bool requires_real_termux_browser_install(std::string_view browser_cmd,
                                          bool is_termux,
                                          bool is_local_mode);

// ---- SSRF toggle -------------------------------------------------------

// Decide whether SSRF protection applies given the backend mode.  SSRF
// only matters for cloud backends; local / Camofox always bypasses.
bool ssrf_protection_applies(bool is_camofox_mode, bool has_cloud_provider);

// Resolve ``allow_private_urls`` from a configured value (may be
// missing).  Default is ``false`` (protection enabled).
bool resolve_allow_private_urls(std::optional<bool> configured);

// ---- Socket-path selection --------------------------------------------

// Choose the temp dir for the agent-browser socket — ``/tmp`` on macOS
// to avoid the 104-byte AF_UNIX limit, otherwise the input fallback.
std::string resolve_browser_tmpdir(std::string_view platform,
                                   std::string_view tmpdir_fallback);

// ---- Screenshot cleanup throttle --------------------------------------

// The tool throttles screenshot-dir cleanup to avoid repeated full
// scans.  The minimum interval between cleanups for the same directory
// (seconds).  Matches ``_last_screenshot_cleanup_by_dir`` guard.
constexpr int kScreenshotCleanupIntervalSeconds = 60;

// Return ``true`` when enough time has elapsed since ``last_cleanup_sec``
// for a fresh cleanup (given ``now_sec``).  When ``last_cleanup_sec`` is
// 0, always returns ``true``.
bool screenshot_cleanup_due(double last_cleanup_sec, double now_sec,
                            int interval_seconds = kScreenshotCleanupIntervalSeconds);

// ---- Snapshot extraction prompts --------------------------------------

// Build the extraction prompt ``_extract_relevant_content`` sends to the
// auxiliary LLM.  When ``user_task`` is non-empty, the task-aware
// variant is used; otherwise the generic variant.
std::string build_snapshot_extraction_prompt(std::string_view snapshot_text,
                                             std::string_view user_task);

// ---- "Empty OK" command allowlist -------------------------------------

// Returns the set of agent-browser subcommands that legitimately
// produce no stdout (close, record).  Callers compare against this to
// avoid flagging an empty response as failure.
const std::vector<std::string>& empty_ok_commands();

// Return ``true`` when ``cmd`` is in ``empty_ok_commands``.
bool is_empty_ok_command(std::string_view cmd);

// ---- Homebrew node discovery ------------------------------------------

// Given a listing of ``/opt/homebrew/opt`` (as a directory name list),
// return the subset that looks like versioned Node.js entries
// (``node@20`` / ``node@24``).  Mirrors ``_discover_homebrew_node_dirs``.
std::vector<std::string> filter_homebrew_node_dirs(
    const std::vector<std::string>& entries);

// ---- Auxiliary-model env lookup ---------------------------------------

// Read an auxiliary-model env var, trimming and returning the value or
// empty when unset.  Matches ``_get_vision_model`` / ``_get_extraction_model``.
std::string trim_env_value(std::string_view raw);

}  // namespace hermes::tools::browser_depth
