// Context-file truncation helper.
//
// Mirrors the Python constants CONTEXT_FILE_MAX_CHARS / _HEAD_RATIO /
// _TAIL_RATIO used by prompt_builder when a discovered AGENTS.md or
// CLAUDE.md file is too large to inline in the system prompt.
#pragma once

#include <cstddef>
#include <string>

namespace hermes::agent {

constexpr std::size_t kContextFileMaxChars = 20000;
constexpr double kContextTruncateHeadRatio = 0.7;
constexpr double kContextTruncateTailRatio = 0.2;

// Truncate `content` to ~kContextFileMaxChars by keeping head + tail
// slices with a "[...truncated N chars]" marker between. No-op when the
// content already fits.
//
// Character accounting is bytes (UTF-8) since std::string stores bytes;
// the ratios are approximate anyway.
std::string truncate_context_file(const std::string& content,
                                  const std::string& filename,
                                  std::size_t max_chars = kContextFileMaxChars,
                                  double head_ratio = kContextTruncateHeadRatio,
                                  double tail_ratio = kContextTruncateTailRatio);

}  // namespace hermes::agent
