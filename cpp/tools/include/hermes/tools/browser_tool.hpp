// Browser tool — registers the ``browser_*`` family with the global tool
// registry.  The handlers thunk through ``BrowserBackend`` (CDP, Camofox,
// remote provider) so the registered handlers are uniform regardless of
// the underlying engine.
//
// Mirrors ``tools/browser_tool.py`` for the registration entry point.
// Pure helpers used by the Python implementation (URL safety, snapshot
// truncation, ref/key parsing) are exposed here so they can be unit
// tested without spinning up a browser.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

void register_browser_tools();

// Maximum chars retained by snapshot truncation (mirror of the Python
// constant in ``_truncate_snapshot``).
constexpr std::size_t kBrowserSnapshotMaxChars = 8000;

// ---- URL safety ----------------------------------------------------------

// Returns ``true`` if ``url`` contains an Anthropic-style ``sk-...``
// prefix (or other vendor token markers) either raw or URL-encoded.  Used
// to refuse navigation to URLs that would exfil secrets in query params.
bool url_contains_secret_marker(std::string_view url);

// Returns ``true`` if ``url`` targets a syntactically loopback or private
// address (RFC1918 IPv4, IPv6 ULA / link-local, ``localhost``, ``.local``
// mDNS, the metadata IP ``169.254.169.254``).  Best-effort string check
// — DNS resolution is intentionally not performed.
bool url_targets_private_address(std::string_view url);

// Convenience wrapper — ``true`` for a URL that is safe to navigate in
// cloud mode (i.e. not a private address and no secret markers).
bool is_safe_browser_url(std::string_view url);

// ---- Page-title heuristics ----------------------------------------------

// Lowercase substring patterns that indicate the page is a CAPTCHA /
// challenge page rather than the requested content.
const std::vector<std::string>& bot_detection_title_patterns();

// Returns ``true`` if ``title`` (case-insensitive) matches any pattern
// in ``bot_detection_title_patterns()``.
bool looks_like_bot_detection(std::string_view title);

// ---- Snapshot helpers ----------------------------------------------------

// Structure-aware snapshot truncation.  Cuts at line boundaries and
// appends a "…N more lines truncated" footer.  Returns the input
// unchanged when below the budget.
std::string truncate_snapshot(std::string_view text,
                              std::size_t max_chars = kBrowserSnapshotMaxChars);

// Extract a screenshot file path embedded in agent-browser stdout.  Tries
// the ``Screenshot saved to "/path/file.png"`` patterns first, then a
// bare absolute ``/...png`` token.  Returns nullopt on no match.
std::optional<std::string> extract_screenshot_path(std::string_view body);

// ---- CDP endpoint normalisation -----------------------------------------

// Normalise a user-supplied CDP endpoint string.  Drops trailing slashes
// and rewrites bare ``ws://host:port`` into ``http://host:port`` so the
// discovery probe can fetch ``/json/version``.  Does NOT perform network
// IO — see ``resolve_cdp_override`` in the runtime layer for the variant
// that hits the discovery endpoint.
std::string normalise_cdp_endpoint(std::string_view raw);

// Parse a snapshot ref string like ``[ref=e5]`` or ``e5`` into the
// raw ref id.  Returns the original (trimmed) string when no ref-style
// token is present.
std::string normalise_browser_ref(std::string_view raw);

// True when ``key`` is an accepted browser_press key descriptor.  Used
// to validate keyboard input before forwarding to the backend.
bool is_known_browser_key(std::string_view key);

// Split a single key descriptor like ``"Ctrl+Shift+A"`` into modifier
// list (lower-cased) and the trailing physical key.  Returns ``{}`` when
// the descriptor is empty.
struct BrowserKeyChord {
    std::vector<std::string> modifiers;
    std::string key;
};
BrowserKeyChord parse_browser_key_chord(std::string_view raw);

}  // namespace hermes::tools
