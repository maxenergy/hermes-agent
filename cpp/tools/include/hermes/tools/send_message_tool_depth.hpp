// Depth-port helpers for ``tools/send_message_tool.py``.  The main
// send_message_tool.hpp/.cpp already exposes parse_target_ref,
// sanitize_error_text, extract_media_directives, and the cron
// auto-delivery check.  This file fills in the remaining pure helpers:
// platform-name normalisation / validation, target-label formatting,
// media classification tables, URL-secret scrubbing, action routing,
// and the "send response" JSON shape the Python code returns.
//
// All helpers are pure — they never touch the gateway, the filesystem,
// or the environment.  They exist so unit tests can pin the exact
// decisions the Python implementation makes without a live platform.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hermes::tools::send_message::depth {

// ---- Action routing -----------------------------------------------------

enum class Action {
    Send,
    List,
    Unknown,
};

// Parse the ``action`` argument.  Accepts ``"send"`` and ``"list"``
// (case-insensitive, surrounding whitespace tolerated).  Empty input
// maps to ``Send`` (the Python default).  Unknown strings return
// ``Unknown`` so callers can surface a tool error.
Action parse_action(std::string_view raw);

// Return the canonical lowercase name for ``action``.  ``Unknown`` maps
// to ``"unknown"``.
std::string action_name(Action action);

// ---- Platform normalisation --------------------------------------------

// The seven platforms the gateway knows how to route to, in the order
// the Python SEND_MESSAGE_SCHEMA lists them.  Returned by reference so
// callers can avoid copying.
const std::vector<std::string>& known_platforms();

// Lowercase, strip-whitespace, and return a canonical platform name.
// Returns the empty string when ``raw`` does not match a known
// platform.  Unlike the wire-level parser, this accepts names only
// (no ``:chat_id`` suffix).
std::string normalise_platform_name(std::string_view raw);

// Return ``true`` when ``name`` (already normalised) matches a known
// platform.
bool is_known_platform(std::string_view name);

// ---- Target parsing (wire-level) ---------------------------------------

struct TargetSpec {
    std::string platform;    // normalised (may be empty if missing)
    std::string remainder;   // everything after the first ':' (may be empty)
    bool has_colon = false;  // true if the raw contained a ':' separator
};

// Split a ``platform[:stuff]`` string without interpreting the
// remainder.  Whitespace around the platform is trimmed and the
// platform is lower-cased via ``normalise_platform_name``.
TargetSpec split_target(std::string_view raw);

// Format a target label used in logs and cron-skip responses.  Returns
// ``platform:chat_id`` or ``platform:chat_id:thread_id`` when the
// thread is present and non-empty.
std::string format_target_label(std::string_view platform,
                                std::string_view chat_id,
                                std::optional<std::string_view> thread_id);

// ---- Media-file classification -----------------------------------------

enum class MediaKind {
    Unknown,
    Image,
    Video,
    Audio,
    Voice,
    Document,
};

// File extensions recognised as each media kind.  Mirrors the
// ``_IMAGE_EXTS`` / ``_VIDEO_EXTS`` / ``_AUDIO_EXTS`` / ``_VOICE_EXTS``
// sets in Python.  All extensions are lowercase and include the
// leading dot.
const std::unordered_set<std::string>& image_extensions();
const std::unordered_set<std::string>& video_extensions();
const std::unordered_set<std::string>& audio_extensions();
const std::unordered_set<std::string>& voice_extensions();

// Classify a single path based on its extension and an ``is_voice``
// hint.  When ``is_voice`` is true and the extension is in the voice
// set, returns ``Voice``.  Otherwise matches against Image/Video/Audio
// sets, falling back to ``Document``.  Empty / extension-less paths
// return ``Document``.
MediaKind classify_media(std::string_view path, bool is_voice);

// Short human-readable label for a MediaKind — matches the
// ``[Sent X attachment]`` / ``[Sent voice message]`` phrasing in
// ``_describe_media_for_mirror``.
std::string media_kind_label(MediaKind kind);

// ---- Secret scrubbing in error text -------------------------------------

// Replace ``access_token=…`` / ``api_key=…`` query parameters in URLs
// with ``…=***``.  Case-insensitive.  Matches the behaviour of
// ``_URL_SECRET_QUERY_RE`` in Python.
std::string scrub_url_query_secrets(std::string_view text);

// Replace ``access_token = 'abc'`` assignment patterns with
// ``access_token=***``.  Case-insensitive, preserves the left-hand
// side verbatim.  Matches ``_GENERIC_SECRET_ASSIGN_RE``.
std::string scrub_generic_secret_assignments(std::string_view text);

// Pipeline: first URL-query scrubbing, then generic assignments.  The
// main sanitize_error_text() in send_message_tool.hpp also prepends a
// redact-sensitive-text pass; this helper exposes the two trailing
// stages on their own so tests can pin each independently.
std::string scrub_secret_patterns(std::string_view text);

// ---- Send-response shapes ----------------------------------------------

// Build the "missing target" error payload.  The Python code returns
// ``{"error": "No target specified. ..."}``.  Exposed so the gateway
// C++ code can match byte-for-byte.
nlohmann::json missing_target_response();

// Build the "missing message" error payload.
nlohmann::json missing_message_response();

// Build the "unknown platform" error payload — names the offending
// platform plus the full list of known platforms for user clarity.
nlohmann::json unknown_platform_response(std::string_view platform);

// Build a success response for a completed send.
nlohmann::json success_response(std::string_view platform,
                                std::string_view chat_id,
                                std::optional<std::string_view> thread_id,
                                std::string_view message);

// Build a cron-duplicate skip response for a target that matches the
// auto-delivery env-var target.  Mirrors the payload emitted by
// ``_maybe_skip_cron_duplicate_send``.
nlohmann::json cron_duplicate_skip_response(std::string_view platform,
                                            std::string_view chat_id,
                                            std::optional<std::string_view> thread_id);

// ---- Channel-directory helpers ------------------------------------------

// Cap a channel-directory listing before emitting to the model.  The
// Python ``_handle_list`` keeps the full list but this helper exposes
// the "cap to N entries" policy used by the gateway's pre-registered
// platform list.  Returns a copy truncated to ``max_entries`` with a
// trailing ``"… N more"`` sentinel when truncation occurs.
std::vector<std::string> cap_channel_list(const std::vector<std::string>& names,
                                          std::size_t max_entries);

// Format a flat ``platform:chat_id`` list as a single newline-joined
// string.  Empty lists return an empty string.
std::string render_channel_list(const std::vector<std::string>& names);

// ---- Telegram topic parsing --------------------------------------------

struct TelegramTopic {
    std::string chat_id;
    std::optional<std::string> thread_id;
    bool matched = false;
};

// Parse a Telegram target reference of the form
// ``<chat_id>[:<thread_id>]``.  ``chat_id`` may be negative.  Returns
// ``matched=false`` on failure.
TelegramTopic parse_telegram_topic(std::string_view ref);

// Parse a Discord target reference — numeric snowflakes in the same
// form as Telegram.  Returns ``matched=false`` when the string is
// non-numeric.
TelegramTopic parse_discord_target(std::string_view ref);

// ---- Env variable lookups (pure wrappers) ------------------------------

// Given a ``key -> value`` snapshot of the environment, return the
// cron auto-delivery target (if any).  Extracted from
// ``_get_cron_auto_delivery_target`` so tests don't need to poke
// getenv.  ``env_lookup`` is a callback; receive a key, return
// nullopt if the variable is unset.
using EnvLookup = std::optional<std::string> (*)(std::string_view);

struct CronAutoTarget {
    std::string platform;
    std::string chat_id;
    std::optional<std::string> thread_id;
};

std::optional<CronAutoTarget> cron_auto_target_from_env(EnvLookup lookup);

}  // namespace hermes::tools::send_message::depth
