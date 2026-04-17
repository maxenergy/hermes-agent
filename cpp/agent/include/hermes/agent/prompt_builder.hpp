// System prompt assembly — agent identity + platform hints + skills
// index + context files + memory.  Mirrors agent/prompt_builder.py but
// accepts pre-scanned inputs (skills, memory, context files) so this
// module stays free of filesystem and network dependencies it does not
// own.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::agent {

struct PromptContext {
    std::string agent_identity;          // blank → DEFAULT_AGENT_IDENTITY
    std::string platform = "cli";        // cli / telegram / discord / slack
    std::filesystem::path cwd;           // optional; used only for display
    std::vector<std::string> memory_entries;   // pre-read from MemoryStore
    std::vector<std::string> user_entries;     // USER.md entries
    std::vector<std::string> skills_index;     // skill name/desc lines
    std::vector<std::string> context_files;    // .hermes.md / HERMES.md bodies
    std::vector<std::string> recent_subdirs;   // from SubdirectoryHintTracker
    bool nous_subscription = false;
    std::string timezone;                // e.g. "Asia/Shanghai"
    // Model identifier — used to decide whether to inject tool-use
    // enforcement guidance (required for GPT/Codex/Gemini/Gemma/Grok,
    // which otherwise stop mid-task to ask the user for confirmation).
    std::string model;
};

class PromptBuilder {
public:
    // Assemble a full system prompt as a single string.  The result is
    // deterministic for a given PromptContext.
    std::string build_system_prompt(const PromptContext& ctx) const;

    // Walk upwards from `cwd` to the git root looking for `.hermes.md` or
    // `HERMES.md`.  Each match is scanned with `is_injection_safe`: safe
    // content is returned verbatim, unsafe content is replaced with a
    // `[BLOCKED: ...]` placeholder.  Frontmatter is stripped.  Returns the
    // discovered bodies in walk order (cwd-first).
    static std::vector<std::string> discover_context_files(
        const std::filesystem::path& cwd);

    // Strip leading YAML frontmatter (delimited by `---`).  Everything
    // before the first non-frontmatter line is removed.
    static std::string strip_frontmatter(std::string_view markdown);

    // Scan for prompt-injection patterns.  Returns TRUE if the content is
    // safe, FALSE if the content contains any known-bad marker.
    //
    // Detected (case-insensitive):
    //   1. "ignore (all|previous) instructions"
    //   2. "new instructions:"
    //   3. "system prompt override"
    //   4. hidden divs: <div style="display:none"...>
    //   5. exfil pipes: `curl ... | sh` / `curl ... | bash`
    //   6. secret reads: `cat *.env`, `grep *.env`
    //   7. SSH backdoor keys: `ssh-rsa AAAA...`
    static bool is_injection_safe(std::string_view content);
};

// Default identity used when PromptContext::agent_identity is empty.
extern const std::string DEFAULT_AGENT_IDENTITY;

// Platform → hint text map.  Key "cli" is always present.  Unknown
// platforms fall back to the "cli" entry.
extern const std::map<std::string, std::string>& platform_hints();

extern const std::string MEMORY_GUIDANCE;
extern const std::string SESSION_SEARCH_GUIDANCE;
extern const std::string SKILLS_GUIDANCE;

}  // namespace hermes::agent
