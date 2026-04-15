// Depth-port helpers for ``tools/web_tools.py`` — pure (no IO) string and
// JSON utilities used by the Python implementation that are safe to exercise
// in unit tests without a live LLM / HTTP backend.  These complement the
// registration layer in ``web_tools.cpp`` and the narrower helpers in
// ``web_tools.hpp``.
//
// The Python module performs a lot of input normalisation: selecting a
// backend from env/config, extracting Firecrawl results across SDK / direct
// / gateway response shapes, chunking oversized content for summarisation,
// stripping base64 images from Markdown, and composing context prefixes.
// All of those are replicated here as pure helpers so the C++ backend can
// match the exact decisions the Python code makes.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::tools::web {

// Upper bound on characters accepted by the summariser — mirrors
// ``MAX_CONTENT_SIZE`` in web_tools.py.  Content over this size is refused
// outright with a friendly error message.
constexpr std::size_t kWebSummariserMaxContent = 2'000'000;

// Content length above which chunked processing kicks in.  Matches
// ``CHUNK_THRESHOLD`` in Python.
constexpr std::size_t kWebSummariserChunkThreshold = 500'000;

// Default chunk size when splitting large content into sections.
constexpr std::size_t kWebSummariserChunkSize = 100'000;

// Minimum length below which the LLM summariser is skipped.
constexpr std::size_t kWebSummariserMinLength = 5'000;

// Hard cap on final summariser output — enforced even after the LLM
// returns.  Matches ``MAX_OUTPUT_SIZE`` in Python.
constexpr std::size_t kWebSummariserMaxOutput = 5'000;

// ---- Backend selection ---------------------------------------------------

enum class WebBackend { Firecrawl, Parallel, Tavily, Exa, Unknown };

// Parse a textual backend name (case-insensitive).  Returns ``Unknown`` for
// anything outside the four officially supported providers.
WebBackend parse_web_backend(std::string_view name);

// Return the canonical lower-case name for ``backend``.  ``Unknown`` maps
// to ``"unknown"``.
std::string web_backend_name(WebBackend backend);

struct BackendAvailability {
    bool firecrawl_key = false;
    bool firecrawl_url = false;
    bool gateway_ready = false;
    bool parallel_key = false;
    bool tavily_key = false;
    bool exa_key = false;
};

// Resolve which backend should be used given a configured preference and
// an availability snapshot.  Mirrors ``_get_backend()`` in Python: honours
// the explicit config, otherwise picks the highest-priority available
// backend, falling back to ``Firecrawl`` as the legacy default.
WebBackend resolve_backend(std::string_view configured,
                           const BackendAvailability& avail);

// Return ``true`` when ``backend`` is currently usable given ``avail``.
bool is_backend_available(WebBackend backend, const BackendAvailability& avail);

// ---- Tavily helpers ------------------------------------------------------

// Normalise a Tavily ``/search`` response envelope into Hermes' standard
// search format: ``{success, data: {web: [{title, url, description,
// position}]}}``.  This matches ``_normalize_tavily_search_results``.
nlohmann::json normalise_tavily_search_results(const nlohmann::json& response);

// Normalise a Tavily ``/extract`` or ``/crawl`` response into the standard
// document list, preserving ``failed_results`` and ``failed_urls`` as
// error entries.  Matches ``_normalize_tavily_documents``.
nlohmann::json normalise_tavily_documents(const nlohmann::json& response,
                                          std::string_view fallback_url = {});

// ---- Result extraction ---------------------------------------------------

// Walk a Firecrawl-style response (SDK object, direct API, or gateway) and
// pull out the first non-empty result list.  Mirrors
// ``_extract_web_search_results`` — checks ``data`` (list), ``data.web``,
// ``data.results``, top-level ``web``, top-level ``results``.
nlohmann::json extract_web_search_results(const nlohmann::json& response);

// Unwrap a Firecrawl scrape payload that may be nested one level under
// ``data``.  Matches ``_extract_scrape_payload``.
nlohmann::json extract_scrape_payload(const nlohmann::json& response);

// Normalise an array of mixed SDK/dict entries into a plain list of JSON
// objects.  Non-object entries are dropped.  Matches
// ``_normalize_result_list``.
nlohmann::json normalise_result_list(const nlohmann::json& values);

// ---- Content preprocessing ----------------------------------------------

// Remove base64-encoded inline images from Markdown content.  Handles
// both parenthesised ``(data:image/...)`` and bare ``data:image/...``
// patterns.  Matches ``clean_base64_images``.
std::string clean_base64_images(std::string_view text);

// Build the ``Title: ...\nSource: ...\n\n`` prefix used by the summariser.
// Either field may be empty; returns an empty string only when both are.
std::string build_summariser_context(std::string_view title,
                                     std::string_view url);

// ---- Chunking for oversize content --------------------------------------

enum class SummariserDecision {
    Refuse,        // content > kWebSummariserMaxContent
    SkipTooShort,  // content < min_length
    SingleShot,    // content fits in one LLM call
    Chunked,       // content needs chunked processing
};

// Decide whether to refuse / skip / run single-shot / chunk.  Uses the
// same thresholds as Python's ``process_content_with_llm``.
SummariserDecision decide_summariser_mode(std::size_t content_len,
                                          std::size_t min_length);

// Split ``content`` into chunks of at most ``chunk_size`` chars.  Matches
// the plain ``for i in range(0, len(content), chunk_size)`` loop in
// ``_process_large_content_chunked``.
std::vector<std::string> split_content_into_chunks(
    std::string_view content,
    std::size_t chunk_size = kWebSummariserChunkSize);

// Compose the ``[Processing chunk N of M]`` banner injected into the
// per-chunk prompt.  Indices are 1-based in the output for human-readable
// messaging, matching Python.
std::string format_chunk_info(std::size_t chunk_index_zero_based,
                              std::size_t total_chunks);

// Cap ``text`` at ``max_output`` chars, appending the standard
// ``"[... truncated ...]"`` footer used by the summariser.  Returns the
// input unchanged when it already fits.
std::string cap_summary_output(std::string_view text,
                               std::size_t max_output = kWebSummariserMaxOutput);

// Build the "content too large" message shown when content exceeds
// ``kWebSummariserMaxContent``.  ``size_bytes`` is formatted to 1 decimal
// place in megabytes.
std::string format_too_large_message(std::size_t size_bytes);

// Build the truncation footer appended when LLM summarisation fails — the
// same message Python emits when falling back to truncated raw content.
std::string format_truncation_footer(std::size_t max_output, std::size_t total_len);

// ---- Environment introspection ------------------------------------------

// Return the default list of env vars Hermes considers when determining
// web-tool availability.  Matches ``_web_requires_env`` with
// ``managed_nous=false``.  When ``managed_nous`` is true the gateway
// variables are appended.  The returned vector is sorted in the same
// insertion order as the Python list so tests can compare ordering.
std::vector<std::string> web_required_env_vars(bool managed_nous);

// Return the URL hostname, lower-cased.  Returns an empty string on
// parse failure.  Used by ``_is_nous_auxiliary_client`` to detect Nous
// Portal base URLs.
std::string url_hostname_lower(std::string_view url);

// Return ``true`` when ``url``'s hostname is ``nousresearch.com`` or a
// subdomain thereof.  Mirrors the Python check.
bool is_nous_auxiliary_base_url(std::string_view url);

// ---- Parallel search-mode parsing ---------------------------------------

// Validate the ``PARALLEL_SEARCH_MODE`` env value.  Accepts ``fast``,
// ``one-shot``, ``agentic`` (case-insensitive).  Defaults to ``agentic``
// like the Python code.
std::string parse_parallel_search_mode(std::string_view raw);

}  // namespace hermes::tools::web
