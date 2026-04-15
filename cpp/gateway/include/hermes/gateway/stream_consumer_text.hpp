// stream_consumer_text — depth port of the text-handling helpers from
// Python gateway/stream_consumer.py (GatewayStreamConsumer).
//
// The core C++ StreamConsumer already handles batching + callback dispatch.
// This module fills in the pure-text / rate-limiting support logic that the
// Python async consumer uses:
//   * MEDIA:<path> / [[audio_as_voice]] tag stripping for display,
//   * overflow splitting that prefers newline boundaries,
//   * streaming cursor + visible-prefix computation,
//   * continuation-text derivation (the tail we still owe the user after a
//     fallback when the platform's edit API fails mid-stream),
//   * rate-limit + buffer-threshold decision logic.
//
// The functions here are pure and fully unit-testable.  They intentionally
// do not touch sockets or asynchronous primitives — the async glue is kept
// in the Python reference implementation until Phase 10.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::gateway {

// ---------------------------------------------------------------------------
// Config mirrors GatewayStreamConsumer.StreamConsumerConfig.
// ---------------------------------------------------------------------------
struct StreamConsumerTextConfig {
    std::chrono::duration<double> edit_interval{0.3};
    std::size_t buffer_threshold = 40;
    std::string cursor = " \u2589";  // "▉"
};

// Strip MEDIA:<path> directives and the [[audio_as_voice]] marker from a
// chunk of streamed text before presenting it to the user.  The function
// mirrors gateway/stream_consumer.py::_clean_for_display exactly:
//   * removes optional surrounding quotes/backticks around the MEDIA tag,
//   * drops [[audio_as_voice]] wherever it appears,
//   * collapses any run of 3+ consecutive newlines down to two,
//   * trims trailing whitespace while preserving leading content.
std::string clean_for_display(std::string_view text);

// True when `text` contains either a MEDIA: tag or the audio marker.  Used
// as a fast-path test (clean_for_display() skips the regex work when this
// returns false).
bool has_media_directives(std::string_view text);

// Return the subset of `final_text` that still needs to be delivered to the
// user given the visible prefix already shown in the streamed message.  The
// helper mirrors GatewayStreamConsumer._continuation_text: when `prefix` is
// empty or not an actual prefix of `final_text`, the full `final_text` is
// returned.  Otherwise the leading prefix and any whitespace separating the
// next chunk is trimmed.
std::string continuation_text(std::string_view final_text,
                              std::string_view prefix);

// Strip the config's streaming cursor from the end of `rendered` and
// return the text actually visible to the user (equivalent to
// GatewayStreamConsumer._visible_prefix).
std::string visible_prefix(std::string_view rendered,
                           std::string_view cursor);

// Split `text` into chunks no larger than `limit`, preferring to split at
// the last newline that falls within the second half of the chunk.  Mirrors
// GatewayStreamConsumer._split_text_chunks.
std::vector<std::string> split_text_chunks(std::string_view text,
                                            std::size_t limit);

// Derive the platform-safe split limit from a raw MAX_MESSAGE_LENGTH,
// reserving room for the streaming cursor and a small formatting buffer.
//   safe_limit = max(500, raw_limit - len(cursor) - 100)
std::size_t safe_split_limit(std::size_t raw_limit,
                             std::string_view cursor);

// Decide whether an edit should be flushed given the current buffer state.
// Mirrors the `should_edit` expression in GatewayStreamConsumer.run.
bool should_edit_now(bool got_done,
                     bool got_segment_break,
                     std::size_t accumulated_len,
                     std::chrono::duration<double> elapsed,
                     const StreamConsumerTextConfig& cfg);

// Build the rendered body for an intermediate edit — the accumulated text
// plus the streaming cursor when neither completion sentinel is present.
std::string render_intermediate_body(std::string_view accumulated,
                                     bool got_done,
                                     bool got_segment_break,
                                     std::string_view cursor);

// Compute the split offset for an over-limit accumulated buffer.
// Mirrors the inner loop in GatewayStreamConsumer.run:
//   split_at = accumulated.rfind('\n', 0, safe_limit);
//   if (split_at < safe_limit / 2) split_at = safe_limit;
std::size_t compute_split_offset(std::string_view accumulated,
                                  std::size_t safe_limit);

}  // namespace hermes::gateway
