// Session search tool — FTS5 full-text search over past sessions.
//
// Mirrors ``tools/session_search_tool.py`` — the production tool also
// summarizes top hits via an auxiliary LLM, but the C++ port currently
// returns raw FTS hits (the summarization layer is deferred to Phase 8
// when async LLM calls land).  This header exposes the helper functions
// the production handler relies on so they can be unit-tested in
// isolation:
//
//   * format_timestamp_human()      — Unix epoch / ISO-8601 → human string.
//   * format_conversation()         — message list → transcript text.
//   * truncate_around_matches()     — bound transcript size, centred on
//                                     the first query-term hit.
//   * parse_role_filter()           — comma-separated role list.
//   * resolve_session_root()        — walk parent_session_id chain to root.
//   * format_recent_session_entry() — list-recent metadata payload.
#pragma once

#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools {

// Maximum chars sent to the auxiliary summarizer per session.  Mirrors
// MAX_SESSION_CHARS in the Python module.
constexpr std::size_t kSessionSearchMaxChars = 100'000;

// Sources hidden from session browsing/searching by default.  Mirrors
// _HIDDEN_SESSION_SOURCES in the Python module.
const std::vector<std::string>& hidden_session_sources();

// ---- Public entry points -------------------------------------------------

void register_session_search_tools(ToolRegistry& registry);

// ---- Helpers (exposed for tests) -----------------------------------------

// Convert a numeric Unix timestamp or ISO-8601 string to a human-friendly
// label like ``"April 13, 2026 at 11:42 AM"``.  Returns "unknown" for
// empty strings and falls back to the original value on parse failure.
std::string format_timestamp_human(const nlohmann::json& ts);

// Convert a list of MessageRows to a single transcript string ready for
// LLM summarization.  Tool outputs longer than 500 chars are truncated
// (head + tail joined with ``...[truncated]...`` to match the Python
// implementation).
std::string format_conversation(
    const std::vector<hermes::state::MessageRow>& messages);

// Centre-and-truncate a transcript to ``max_chars`` around the first
// occurrence of any query term.  When the text is already short enough
// it is returned unchanged.
std::string truncate_around_matches(const std::string& text,
                                    const std::string& query,
                                    std::size_t max_chars = kSessionSearchMaxChars);

// Split a comma-separated role filter into a normalised vector.  Empty
// input returns an empty vector.  Unknown roles are kept; callers
// validate them.
std::vector<std::string> parse_role_filter(std::string_view raw);

// Walk parent_session_id pointers up to the root.  ``loader`` returns
// ``{"parent_session_id": "...", "ok": true}`` for known ids; any other
// shape stops the walk.  Used to dedupe child sessions created by
// delegation/compression so the search returns unique top-level
// conversations.
std::string resolve_session_root(
    const std::string& session_id,
    const std::function<nlohmann::json(const std::string&)>& loader);

// Build the metadata entry returned from the "recent sessions" mode.
nlohmann::json format_recent_session_entry(const hermes::state::SessionRow& s,
                                           std::string_view preview);

// Run the recent-sessions branch of the tool.  Pulled out so unit tests
// can exercise it without spinning up the full registry/dispatch path.
nlohmann::json list_recent_sessions(hermes::state::SessionDB& db,
                                    int limit,
                                    const std::string& current_session_id = "");

// True if the given source string should be hidden from session
// browsing/searching by default.
bool is_hidden_source(std::string_view source);

}  // namespace hermes::tools
