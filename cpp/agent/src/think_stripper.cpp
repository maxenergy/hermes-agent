#include "hermes/agent/think_stripper.hpp"

#include <regex>

namespace hermes::agent {

namespace {

// ECMAScript `.` does not match newlines, so we substitute `[\s\S]` in
// place of `.*?` / `.*` below. `std::regex::icase` provides the
// case-insensitive equivalent of Python's re.IGNORECASE.
constexpr auto kFlags = std::regex::ECMAScript | std::regex::icase;

}  // namespace

std::string strip_think_blocks(const std::string& content) {
    if (content.empty()) {
        return {};
    }

    // 1. Closed tag pairs — non-greedy, case-insensitive across all
    //    variants so mixed-case tags (<THINK>, <Thinking>) don't slip
    //    through to the unterminated-tag pass below and take trailing
    //    visible content with them.
    static const std::regex re_think(R"(<think>[\s\S]*?</think>)", kFlags);
    static const std::regex re_thinking(R"(<thinking>[\s\S]*?</thinking>)",
                                        kFlags);
    static const std::regex re_reasoning(R"(<reasoning>[\s\S]*?</reasoning>)",
                                         kFlags);
    static const std::regex re_scratchpad(
        R"(<REASONING_SCRATCHPAD>[\s\S]*?</REASONING_SCRATCHPAD>)", kFlags);
    static const std::regex re_thought(R"(<thought>[\s\S]*?</thought>)",
                                       kFlags);

    // 2. Unterminated reasoning block — open tag at a block boundary
    //    (start of text, or after a newline, possibly indented with
    //    spaces/tabs) with no matching close. Strip from the tag to
    //    end of string. Matches gateway/stream_consumer.py's boundary
    //    check so models that mention ``<think>`` in prose are not
    //    over-stripped.
    static const std::regex re_unterminated(
        R"((?:^|\n)[ \t]*<(?:think|thinking|reasoning|thought|REASONING_SCRATCHPAD)\b[^>]*>[\s\S]*$)",
        kFlags);

    // 3. Stray orphan open/close tags that slipped through both prior
    //    passes (e.g. a lone </think> with no matching open).
    static const std::regex re_orphan(
        R"(</?(?:think|thinking|reasoning|thought|REASONING_SCRATCHPAD)>\s*)",
        kFlags);

    std::string s = content;
    s = std::regex_replace(s, re_think, "");
    s = std::regex_replace(s, re_thinking, "");
    s = std::regex_replace(s, re_reasoning, "");
    s = std::regex_replace(s, re_scratchpad, "");
    s = std::regex_replace(s, re_thought, "");
    s = std::regex_replace(s, re_unterminated, "");
    s = std::regex_replace(s, re_orphan, "");
    return s;
}

}  // namespace hermes::agent
