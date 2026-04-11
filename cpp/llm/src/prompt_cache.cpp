// Anthropic prompt-cache injection (system_and_3 strategy).
//
// Port of agent/prompt_caching.py.  Unlike the Python version we operate
// in-place on the caller-supplied std::vector<Message> — the spec
// demands:
//   * Never mutates past context beyond adding cache_control markers.
//   * Max 4 breakpoints total (Anthropic hard limit).
//   * Idempotent: calling twice produces the same result.
//   * Operates in place — does not deep-copy.
//
// The "idempotent" invariant holds because we OVERWRITE existing
// cache_control on the target blocks with the same marker value.
#include "hermes/llm/prompt_cache.hpp"

#include <cstddef>
#include <vector>

namespace hermes::llm {

namespace {

nlohmann::json make_marker(const std::string& ttl) {
    nlohmann::json marker;
    marker["type"] = "ephemeral";
    if (ttl == "1h") {
        marker["ttl"] = "1h";
    }
    return marker;
}

// Add the marker to the message.  Mirrors Python _apply_cache_marker:
//   * empty content → set Message::cache_control
//   * simple text → promote into a single ContentBlock with marker
//   * block list → attach to the LAST block
void apply_marker(Message& msg, const nlohmann::json& marker) {
    // Role::Tool handling in the Python helper only sets top-level
    // cache_control when native_anthropic is true — we are only called
    // from the native_anthropic=true branch below, so just mirror that.
    if (msg.role == Role::Tool) {
        msg.cache_control = marker;
        return;
    }

    if (msg.content_blocks.empty()) {
        if (msg.content_text.empty()) {
            // No content to attach to — fall back to top-level marker.
            msg.cache_control = marker;
            return;
        }
        // Promote textual content into a single block so the marker has
        // somewhere to live (Anthropic content blocks are per-block).
        ContentBlock b;
        b.type = "text";
        b.text = msg.content_text;
        b.cache_control = marker;
        msg.content_blocks.push_back(std::move(b));
        msg.content_text.clear();
        return;
    }

    // Attach to the last block — critical invariant for the test suite.
    msg.content_blocks.back().cache_control = marker;
}

}  // namespace

void apply_anthropic_cache_control(std::vector<Message>& messages,
                                   const PromptCacheOptions& opts) {
    if (!opts.native_anthropic) {
        // Non-Anthropic providers (OpenAI / OpenRouter) don't honour
        // cache_control, so leave the messages untouched.
        return;
    }
    if (messages.empty()) return;

    const auto marker = make_marker(opts.cache_ttl);

    // Max 4 breakpoints total.
    int breakpoints_used = 0;
    if (messages.front().role == Role::System) {
        apply_marker(messages.front(), marker);
        ++breakpoints_used;
    }

    const int remaining = 4 - breakpoints_used;
    // Collect non-system indices in order.
    std::vector<std::size_t> non_sys;
    non_sys.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != Role::System) {
            non_sys.push_back(i);
        }
    }

    // Take the last `remaining` entries.
    const std::size_t take = (non_sys.size() <= static_cast<std::size_t>(remaining))
                                 ? non_sys.size()
                                 : static_cast<std::size_t>(remaining);
    const std::size_t start = non_sys.size() - take;
    for (std::size_t k = start; k < non_sys.size(); ++k) {
        apply_marker(messages[non_sys[k]], marker);
    }
}

}  // namespace hermes::llm
