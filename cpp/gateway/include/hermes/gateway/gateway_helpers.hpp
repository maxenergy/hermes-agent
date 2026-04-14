// Stateless helpers shared by the gateway pipeline, adapters and tests.
//
// Ports small pure-function utilities from gateway/run.py and
// gateway/platforms/base.py that are too thin to deserve their own module:
//
//   - build_media_placeholder   (for text-less media messages)
//   - safe_url_for_log          (redact query / mask tokens)
//   - truncate_message          (split into platform-safe chunks)
//   - human_delay               (typing delay jitter)
//
// Everything here is pure C++ — no adapter or I/O side effects.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::gateway {

// Minimal payload describing a media-only event.  Used to format a
// ``[user sent a photo]``-style placeholder for the agent.
struct MediaPlaceholderEvent {
    std::string message_type;   // PHOTO | VIDEO | AUDIO | VOICE | DOCUMENT | STICKER
    std::vector<std::string> media_urls;
    std::string caption;        // optional user caption (kept verbatim)
};

// Build the text blob the agent should see when picking up a pending
// media-only event.  Mirrors gateway/run.py::_build_media_placeholder.
std::string build_media_placeholder(const MediaPlaceholderEvent& event);

// Lightweight variant: given just the message_type and a single URL, emit
// the placeholder used for queued media messages.
std::string build_media_placeholder(std::string_view message_type,
                                     std::string_view url);

// Return a log-safe representation of ``url``: scheme + host + up to
// ``max_len`` characters of path, with all query parameters stripped.
// Matches gateway/platforms/base.py::safe_url_for_log.
std::string safe_url_for_log(std::string_view url, std::size_t max_len = 80);

// Mask a bearer-style token so it is safe to log or echo back to the
// user.  Preserves the first 4 and last 4 characters; the middle is
// replaced with ``***``.  Returns ``***`` for tokens shorter than 10.
std::string mask_token(std::string_view token);

// Truncate ``content`` into chunks no longer than ``max_length``,
// breaking on paragraph / sentence boundaries when possible.  Used by
// Telegram (4096), Discord (2000), etc.  Returns at least one chunk;
// empty input yields an empty vector.
std::vector<std::string> truncate_message(std::string_view content,
                                           std::size_t max_length);

// Return a human-like typing delay (jittered 0.8s..2.4s) derived from
// ``seed``.  When ``seed`` is zero, a time-derived seed is used.  The
// deterministic form is used from tests.
std::chrono::milliseconds human_delay(std::uint64_t seed = 0);

// Normalize a chat identifier: trim whitespace, lowercase scheme-like
// prefixes ("telegram:12345" -> "telegram:12345").  Returns empty when
// the input is empty or contains control characters.
std::string normalize_chat_id(std::string_view raw);

// Detect whether ``content`` looks like a slash command (``/cmd ...``).
// Ignores leading whitespace.  Does NOT enforce command whitelists —
// the dispatcher handles that.
bool looks_like_slash_command(std::string_view content);

// Return the command word for a slash command (without the leading
// slash), lowercased.  Returns empty optional when content is not a
// slash command.
std::optional<std::string> extract_command_word(std::string_view content);

// Extract the positional arguments for a slash command (everything
// after the first whitespace, trimmed).
std::string extract_command_args(std::string_view content);

// Percent-encode ``value`` for safe inclusion in a URL path / query.
std::string percent_encode(std::string_view value);

// Return ``true`` if two content strings are "equivalent" for
// deduplication purposes (case-insensitive + whitespace-collapsed).
bool contents_equivalent(std::string_view a, std::string_view b);

}  // namespace hermes::gateway
