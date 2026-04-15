// Extra depth helpers for tools/web_tools.py — pure helpers that extend
// web_tools_depth.hpp with additional normalisation surface the Python
// implementation relies on.  The original file focused on backend
// selection, chunking, and the Tavily adapter; this file adds: result
// envelope unwrapping for SDK-returned objects, document-record sanity
// checks, Parallel/Exa query-body assembly, Firecrawl error-shape
// detection, user-agent / rate-limit-retry-after parsing, and the
// query-size caps enforced before any HTTP call.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools::web::depth_ex {

// ---- Query-size caps ---------------------------------------------------

// Maximum characters allowed in a single search query.  Matches the
// soft cap Python enforces before hitting Firecrawl.  Queries above
// this length produce a user-facing error.
constexpr std::size_t kMaxQueryChars = 512;

// Maximum number of concurrent URL-extract batch members.  Matches the
// parallel batch cap used by ``_extract_web_search_results`` callers.
constexpr std::size_t kMaxBatchUrls = 20;

// ---- Query validation ---------------------------------------------------

// Validate a search query.  Returns an error string when invalid,
// empty on success.  Rejects empty / whitespace-only / oversized
// queries.
std::string validate_query(std::string_view query);

// Validate a list of URLs for a batch-extract call.  Returns an error
// string on the first URL that fails basic shape checks or when the
// batch is empty / oversized.
std::string validate_url_batch(const std::vector<std::string>& urls);

// Return ``true`` when ``url`` looks like a usable HTTP(S) URL.  Very
// permissive — Python's full validator is hostname-aware but the soft
// pre-flight check is just scheme + non-empty host.
bool is_http_url(std::string_view url);

// ---- Envelope unwrapping ------------------------------------------------

// Unwrap a nested ``data`` field when present.  Matches the SDK
// adapter pattern where responses have the shape
// ``{"data": {...}}``.  Non-object inputs pass through unchanged.
nlohmann::json unwrap_data(const nlohmann::json& response);

// Extract a list-of-objects field from a response under one of the
// candidate key paths.  Returns an empty array when no key matches.
// ``keys`` is searched in order; nested paths use ``"."`` separators
// (e.g., ``"data.web"``).
nlohmann::json pluck_first_array(const nlohmann::json& response,
                                 const std::vector<std::string>& keys);

// Coerce a possibly-SDK value to a plain JSON object by copying its
// top-level keys.  Non-object / non-mapping inputs return ``null``.
// Exposed for tests that want to pin the "plain-ify" behaviour.
nlohmann::json to_plain_object(const nlohmann::json& value);

// ---- Document record sanity checks -------------------------------------

// Return ``true`` when ``doc`` has the minimum fields required for a
// normalised document (non-empty url, content, title may be empty).
bool document_is_usable(const nlohmann::json& doc);

// Strip fields the summariser never uses: ``raw_content``, ``html``,
// ``screenshot``, ``markdown``, ``links`` — anything that bloats the
// context without informing the response.  Returns a copy.
nlohmann::json strip_noise_fields(const nlohmann::json& doc);

// ---- Parallel / Exa query body ----------------------------------------

// Build the Parallel ``/search`` request body.  ``search_mode`` is
// expected to be one of ``fast``/``one-shot``/``agentic`` (already
// validated); ``max_results`` is capped at 20.
nlohmann::json build_parallel_search_body(std::string_view query,
                                          std::string_view search_mode,
                                          std::size_t max_results);

// Build the Exa ``/search`` request body.  ``use_autoprompt`` toggles
// the LLM-powered query rewriting Exa performs server-side.
nlohmann::json build_exa_search_body(std::string_view query,
                                     std::size_t max_results,
                                     bool use_autoprompt);

// ---- Firecrawl error-shape detection -----------------------------------

// Return an error string when ``response`` is a Firecrawl-style error
// envelope, else empty.  Matches the error checks in ``_firecrawl_*``
// wrappers.
std::string firecrawl_error_message(const nlohmann::json& response);

// ---- HTTP-header helpers -----------------------------------------------

// Parse a ``Retry-After`` header value (seconds or HTTP-date).  Only
// the seconds form is supported here (the HTTP-date path is handled
// by the runtime HTTP layer).  Returns nullopt on parse failure.
std::optional<int> parse_retry_after_seconds(std::string_view header);

// Return the default User-Agent string the web tools advertise.  Pure
// so tests don't have to import the full build-info module.
std::string default_user_agent();

// ---- Summariser prompt assembly ---------------------------------------

// Compose the chunked-mode summariser prompt prefix.  Matches the
// Python f-string in ``_process_large_content_chunked``.
std::string chunk_prompt_prefix(std::string_view title,
                                std::string_view source_url,
                                std::size_t chunk_index,
                                std::size_t total_chunks);

// Compose the single-shot summariser prompt prefix.  Matches Python's
// single-shot branch in ``process_content_with_llm``.
std::string single_shot_prompt_prefix(std::string_view title,
                                      std::string_view source_url);

}  // namespace hermes::tools::web::depth_ex
