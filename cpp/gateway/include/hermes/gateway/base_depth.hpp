// Depth port of pure helpers from gateway/platforms/base.py.
//
// The existing base_adapter.hpp covers the stateful mixin (connection
// state, rate limits, retry budgets, feature flags).  This header adds
// the standalone utilities that the Python module exposes at module
// scope: URL/proxy resolution, UTF-16 length accounting (Telegram's
// 4 096-code-unit cap), media/markdown extraction, command parsing on
// ``MessageEvent``, retry/timeout classification, caption merging, and
// fence-aware message splitting (``truncate_message``).
//
// These helpers are intentionally free functions so they can be used
// from adapter code, tests, and command-line tooling without dragging
// in the full adapter lifecycle.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hermes::gateway::base_depth {

// --- UTF-16 accounting ---------------------------------------------------

// Count UTF-16 code units in *s*.
//
// Telegram measures its 4 096-character limit in UTF-16 code units, not
// code points.  Characters outside the Basic Multilingual Plane (emoji,
// CJK Extension B, musical symbols) are encoded as surrogate pairs and
// therefore consume two UTF-16 code units even though Python counts
// them as one.
std::size_t utf16_len(std::string_view s);

// Return the longest prefix of *s* whose UTF-16 length is <= ``limit``.
// Respects surrogate-pair boundaries so we never slice a multi-code-unit
// character in half.
std::string prefix_within_utf16_limit(std::string_view s, std::size_t limit);

// --- URL sanitization ----------------------------------------------------

// Return a URL string safe for logs (no query/fragment/userinfo).
// Hostnames with embedded credentials (``user:pass@host``) are reduced
// to the bare host; long URLs are elided with ``.../<basename>`` and a
// final ``...`` truncation to ``max_len``.
std::string safe_url_for_log(std::string_view url, std::size_t max_len = 80);

// Lightweight URL parser — returns (scheme, netloc, path).  Only handles
// the subset used by :func:`safe_url_for_log`.  Does not decode percent
// escapes or resolve relative references.
struct ParsedUrl {
    std::string scheme;
    std::string netloc;
    std::string path;
    bool has_scheme_netloc = false;
};
ParsedUrl parse_url(std::string_view url);

// --- Proxy resolution ----------------------------------------------------

// Resolve a proxy URL by checking ``platform_env_var`` (if non-empty),
// then the standard ``HTTPS_PROXY`` / ``HTTP_PROXY`` / ``ALL_PROXY``
// chain (upper- and lower-case variants).  Returns an empty optional if
// none are set.  ``getenv_fn`` is injected so tests can simulate the
// environment without touching the process environment.
using GetEnvFn = std::string (*)(const char*);
std::optional<std::string> resolve_proxy_url(
    std::string_view platform_env_var,
    GetEnvFn getenv_fn);

// Classify a proxy URL.  Returns ``Socks`` for ``socks://``/``socks4``/
// ``socks5``/``socks5h``; ``Http`` for ``http://``/``https://``;
// ``Unknown`` otherwise.
enum class ProxyKind {
    None,
    Http,
    Socks,
    Unknown,
};
ProxyKind classify_proxy(std::string_view url);

// --- Image / media detection --------------------------------------------

// Return true if ``data`` starts with a known image magic-byte sequence
// (PNG, JPEG, GIF87a/GIF89a, BMP, WEBP).
bool looks_like_image(const unsigned char* data, std::size_t len);
inline bool looks_like_image(std::string_view data) {
    return looks_like_image(
        reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

// Return true if ``url`` points to an animated GIF (strips query string,
// then tests for a ``.gif`` suffix).
bool is_animation_url(std::string_view url);

// Extract ``![alt](url)`` and ``<img src="url">`` markdown/HTML images
// from ``content``.  Returns a list of ``(url, alt)`` pairs plus a
// cleaned copy of ``content`` with the matched image tags removed.
// Markdown images are filtered to those whose URL ends in a known image
// extension or contains one of the well-known CDN markers.
std::pair<std::vector<std::pair<std::string, std::string>>, std::string>
extract_images(std::string_view content);

// Extract ``MEDIA:<path>`` tags and the ``[[audio_as_voice]]``
// directive from ``content``.  Returns ``(pairs, cleaned)`` where each
// pair is ``(path, is_voice)`` and ``is_voice`` reflects whether the
// directive was present anywhere in the input.
std::pair<std::vector<std::pair<std::string, bool>>, std::string>
extract_media(std::string_view content);

// Extract bare absolute / home-relative local file paths ending in a
// known image/video extension from ``content``.  Paths inside code
// fences (```` ``` ... ``` ````) or inline backticks are skipped.  This
// version does **not** call ``isfile()`` — it returns every candidate
// so the caller can check the filesystem (or not, as tests prefer).
std::pair<std::vector<std::string>, std::string>
extract_local_files_raw(std::string_view content);

// --- Command parsing -----------------------------------------------------

// Return true if ``text`` starts with a slash.  Mirrors
// ``MessageEvent.is_command``.
bool is_command(std::string_view text);

// Extract the command name from ``text`` (e.g. "/reset arg1 arg2" →
// "reset").  Returns an empty optional when the string is not a
// command, when the candidate name contains a slash (reject file
// paths), or when parsing fails.  The ``@mention`` suffix used by
// Telegram bots is stripped.
std::optional<std::string> get_command(std::string_view text);

// Return the argument substring after a command.  For a non-command
// message, returns the original text.
std::string get_command_args(std::string_view text);

// --- Retry / timeout classification -------------------------------------

// Return true when the error text matches one of the transient network
// patterns that are safe to retry (``connecttimeout``, ``network``,
// ``connectionreset`` …).  Notably excludes ``timed out`` /
// ``readtimeout`` / ``writetimeout`` which are non-idempotent.
bool is_retryable_error(std::string_view error_text);

// Return true when the error text indicates a read/write timeout.
// These are *not* retryable because the request may have reached the
// server — retrying risks duplicate delivery.
bool is_timeout_error(std::string_view error_text);

// --- Caption merge -------------------------------------------------------

// Merge a new caption into existing text, avoiding duplicate lines.
// Uses line-by-line exact match (after whitespace trim) rather than
// substring match so shorter captions are not silently dropped when
// they appear inside a longer one.
std::string merge_caption(std::string_view existing_text,
                           std::string_view new_text);

// --- Truncate with fences -----------------------------------------------

// Split a long message into chunks while preserving triple-backtick
// code-block boundaries.  When a split falls inside a fenced block, the
// fence is closed at the end of the current chunk and reopened (with
// the original language tag) at the start of the next.  Multi-chunk
// outputs receive ``(i/N)`` indicators appended to each piece.
std::vector<std::string> truncate_message(std::string_view content,
                                            std::size_t max_length = 4096);

// --- Network-accessible host --------------------------------------------

// Return true if ``host`` would expose the server beyond loopback.
// Numeric loopback addresses (127.0.0.1, ::1, ::ffff:127.0.0.1) and
// host literals resolving only to loopback are treated as
// non-accessible; anything else is considered externally reachable.
// When DNS resolution fails the function fails closed (returns false).
bool is_network_accessible(std::string_view host);

// Parse a bare numeric address.  Returns the family (4, 6) and whether
// it is a loopback / unspecified / mapped-loopback address.  Parsing
// failure returns ``Invalid``.
enum class AddressKind {
    Invalid,
    Ipv4Loopback,
    Ipv4Unspecified,
    Ipv4Public,
    Ipv6Loopback,
    Ipv6Unspecified,
    Ipv6MappedLoopback,
    Ipv6Public,
};
AddressKind classify_address(std::string_view host);

}  // namespace hermes::gateway::base_depth
