// context_references — user-registered files / URLs / snippets that
// should be visible to the model alongside the live conversation.
//
// Two injection points are supported, matching Python's reference model:
//
//   * Stable refs (kind=File|Url with `stable=true`) are rendered ONCE
//     into the system prompt as fixed `<context ref="...">…</context>`
//     blocks.  Because they're stable, they do NOT invalidate the
//     Anthropic prompt cache when the conversation continues.
//
//   * Per-turn refs (`stable=false`) are emitted as a user-role message
//     inserted BEFORE the next user turn.  The active conversation
//     history is untouched — we never rewrite past messages.
//
// The canonical slash-command entry point is `/ref add <path|url>`
// (wired in cpp/cli/src/commands.cpp).  Programmatic callers use
// register_file / register_url / register_snippet directly.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hermes::agent {

enum class ContextRefKind {
    File,
    Url,
    Snippet,
};

struct ContextReference {
    ContextRefKind kind = ContextRefKind::File;
    std::string source;          // path string / URL / snippet id
    std::string content;         // already-loaded body (UTF-8)
    std::chrono::system_clock::time_point loaded_at{};
    bool stable = true;          // stable → system-prompt injection
    std::string label;           // human label (optional)
};

class ContextReferences {
public:
    // Upper bound on content length retained per reference; content
    // beyond this is truncated with a "…[truncated]" marker so a huge
    // file can't silently blow up the prompt.
    static constexpr std::size_t kMaxContentBytes = 64 * 1024;

    ContextReferences() = default;

    // Load `path` into a File reference.  Throws std::runtime_error if
    // the file can't be opened.  `stable` controls injection point.
    // Returns the newly-added reference.
    const ContextReference& register_file(const std::filesystem::path& path,
                                          bool stable = true);

    // Register a URL reference.  This module does NOT fetch the URL;
    // the caller passes already-fetched content (typically from a
    // WebFetch tool).  Keeps network concerns out of the agent layer.
    const ContextReference& register_url(const std::string& url,
                                         std::string content,
                                         bool stable = true);

    // Register a raw snippet.  `source` is the caller-supplied id.
    const ContextReference& register_snippet(const std::string& source,
                                             std::string content,
                                             bool stable = false);

    // Remove a reference by (kind, source) tuple.  Returns true if a
    // matching entry was removed.
    bool remove(ContextRefKind kind, const std::string& source);

    // Drop every reference (used by /clear and session reset).
    void clear();

    // Inspection.
    std::vector<ContextReference> list() const;
    std::size_t size() const;
    bool empty() const;

    // Render the stable references as a single block suitable for
    // appending to the system prompt.  Returns an empty string when
    // there are no stable refs.
    std::string render_stable_block() const;

    // Drain the pending per-turn (non-stable) refs into a single user
    // message body.  After calling, the non-stable refs are cleared so
    // they're not re-injected on the next turn.  Returns empty when no
    // per-turn refs are pending.
    std::string drain_per_turn_block();

    // XML-escape helper (lowercase name for consistency with nlohmann
    // utilities).  Exposed for tests.
    static std::string xml_escape(const std::string& in);

private:
    mutable std::mutex mu_;
    std::vector<ContextReference> refs_;

    static std::string render_one(const ContextReference& r);
};

}  // namespace hermes::agent
