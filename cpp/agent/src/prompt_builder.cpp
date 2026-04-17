#include "hermes/agent/prompt_builder.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace hermes::agent {

namespace fs = std::filesystem;

const std::string DEFAULT_AGENT_IDENTITY =
    "You are Hermes, an AI agent built to help users accomplish real work "
    "on their computer. You operate through a tool-driven loop: read the "
    "request, plan, call tools, observe results, and continue until the "
    "task is done. Keep responses focused and avoid speculation.";

const std::string MEMORY_GUIDANCE =
    "## Memory\n"
    "You have access to a long-lived MEMORY.md (project-level) and "
    "USER.md (user-level) memory file.  Use the `memory` tool to add or "
    "edit entries — never invent facts about the user that you cannot "
    "back up from the conversation.\n";

const std::string SESSION_SEARCH_GUIDANCE =
    "## Session search\n"
    "Use the `session_search` tool to recall relevant prior conversations "
    "when the user references something you do not see in the live "
    "context window.\n";

const std::string SKILLS_GUIDANCE =
    "## Skills\n"
    "Skills are additional, opt-in capability bundles.  When a user's "
    "request matches a skill's trigger, prefer the skill's documented "
    "workflow over ad-hoc tool calls.\n";

// Strong autonomy guidance — ported from agent/prompt_builder.py.
// Without this, GPT/Codex/Gemini/Gemma/Grok tend to stop mid-task and
// ask the user "shall I continue?" after every substantive step.
const std::string TOOL_USE_ENFORCEMENT_GUIDANCE =
    "# Tool-use enforcement\n"
    "You MUST use your tools to take action — do not describe what you "
    "would do or plan to do without actually doing it. When you say you "
    "will perform an action (e.g. 'I will run the tests', 'Let me check "
    "the file', 'I will create the project'), you MUST immediately make "
    "the corresponding tool call in the same response. Never end your "
    "turn with a promise of future action — execute it now.\n"
    "Keep working until the task is actually complete. Do not stop with "
    "a summary of what you plan to do next time. If you have tools "
    "available that can accomplish the task, use them instead of telling "
    "the user what you would do.\n"
    "Every response should either (a) contain tool calls that make "
    "progress, or (b) deliver a final result to the user. Responses that "
    "only describe intentions without acting are not acceptable.\n";

// GPT/Codex-specific execution discipline.  Addresses GPT-5.x failure
// modes: bailing on partial results, skipping prerequisite lookups,
// hallucinating instead of using tools, and declaring "done" without
// verification.
const std::string OPENAI_MODEL_EXECUTION_GUIDANCE =
    "# Execution discipline\n"
    "<tool_persistence>\n"
    "- Use tools whenever they improve correctness, completeness, or "
    "grounding.\n"
    "- Do not stop early when another tool call would materially improve "
    "the result.\n"
    "- If a tool returns empty or partial results, retry with a different "
    "query or strategy before giving up.\n"
    "- Keep calling tools until: (1) the task is complete, AND (2) you "
    "have verified the result.\n"
    "</tool_persistence>\n"
    "\n"
    "<act_dont_ask>\n"
    "When a question has an obvious default interpretation, act on it "
    "immediately instead of asking for clarification. Examples:\n"
    "- 'Is port 443 open?' → check THIS machine (don't ask 'open where?')\n"
    "- 'What OS am I running?' → check the live system (don't use user "
    "profile)\n"
    "- 'What time is it?' → run `date` (don't guess)\n"
    "Only ask for clarification when the ambiguity genuinely changes "
    "what tool you would call.\n"
    "</act_dont_ask>\n"
    "\n"
    "<mandatory_tool_use>\n"
    "NEVER answer these from memory — ALWAYS use a tool:\n"
    "- Arithmetic, math, calculations → terminal or execute_code\n"
    "- Hashes, encodings, checksums → terminal (sha256sum, base64, etc.)\n"
    "- Current time, date, timezone → terminal (`date`)\n"
    "- System state: OS, CPU, memory, disk, ports, processes → terminal\n"
    "- File contents, sizes, line counts → read_file / search_files / "
    "terminal\n"
    "- Git history, branches, diffs → terminal\n"
    "- Current facts (weather, news, versions) → web_search\n"
    "Your memory and user profile describe the USER, not the system you "
    "are running on.\n"
    "</mandatory_tool_use>\n"
    "\n"
    "<verification>\n"
    "Before finalizing:\n"
    "- Correctness: does the output satisfy every stated requirement?\n"
    "- Grounding: are factual claims backed by tool outputs?\n"
    "- Formatting: does the output match the requested schema?\n"
    "- Safety: if the next step has side effects, confirm scope before "
    "executing destructive operations.\n"
    "</verification>\n";

// Gemini/Gemma-specific operational guidance, adapted from OpenCode's
// gemini.txt.  Injected alongside TOOL_USE_ENFORCEMENT_GUIDANCE when the
// model is Gemini or Gemma.  Grok is not Google — it only gets the base
// enforcement text.
const std::string GOOGLE_MODEL_OPERATIONAL_GUIDANCE =
    "# Google model operational directives\n"
    "Follow these operational rules strictly:\n"
    "- **Absolute paths:** Always construct and use absolute file paths for all "
    "file system operations. Combine the project root with relative paths.\n"
    "- **Verify first:** Use read_file/search_files to check file contents and "
    "project structure before making changes. Never guess at file contents.\n"
    "- **Dependency checks:** Never assume a library is available. Check "
    "package.json, requirements.txt, Cargo.toml, etc. before importing.\n"
    "- **Conciseness:** Keep explanatory text brief — a few sentences, not "
    "paragraphs. Focus on actions and results over narration.\n"
    "- **Parallel tool calls:** When you need to perform multiple independent "
    "operations (e.g. reading several files), make all the tool calls in a "
    "single response rather than sequentially.\n"
    "- **Non-interactive commands:** Use flags like -y, --yes, --non-interactive "
    "to prevent CLI tools from hanging on prompts.\n"
    "- **Keep going:** Work autonomously until the task is fully resolved. "
    "Don't stop with a plan — execute it.\n";

namespace {

// Return true if `model` looks like a family that needs explicit
// tool-use enforcement.  Matches the Python
// ``TOOL_USE_ENFORCEMENT_MODELS`` tuple.
bool model_needs_enforcement(const std::string& model) {
    if (model.empty()) return false;
    std::string m = model;
    for (auto& c : m) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    static const char* kPatterns[] = {"gpt", "codex", "gemini", "gemma",
                                       "grok"};
    for (const char* p : kPatterns) {
        if (m.find(p) != std::string::npos) return true;
    }
    return false;
}

// True when the model is in the OpenAI GPT / Codex family — gets the
// extra OPENAI_MODEL_EXECUTION_GUIDANCE block on top of the base
// enforcement text.
bool model_is_openai_family(const std::string& model) {
    if (model.empty()) return false;
    std::string m = model;
    for (auto& c : m) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    return m.find("gpt") != std::string::npos ||
           m.find("codex") != std::string::npos;
}

// True when the model is in the Google Gemini / Gemma family — gets the
// extra GOOGLE_MODEL_OPERATIONAL_GUIDANCE block on top of the base
// enforcement text.  Grok is xAI, not Google, so it is excluded.
bool model_is_google_family(const std::string& model) {
    if (model.empty()) return false;
    std::string m = model;
    for (auto& c : m) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    return m.find("gemini") != std::string::npos ||
           m.find("gemma") != std::string::npos;
}

const std::map<std::string, std::string> kPlatformHints = {
    {"cli",
     "## Platform: CLI\n"
     "You are running inside the `hermes` command-line interface.  The "
     "user can see streamed output in their terminal.  Prefer compact, "
     "scannable answers.\n"},
    {"telegram",
     "## Platform: Telegram\n"
     "You are running inside a Telegram bot.  Replies are rendered as "
     "Markdown — do NOT use HTML.  Keep messages short and split long "
     "replies across multiple sends.\n"},
    {"discord",
     "## Platform: Discord\n"
     "You are running inside a Discord bot.  Replies are rendered as "
     "Markdown.  Use code fences for source code and inline backticks "
     "for command names.\n"},
    {"slack",
     "## Platform: Slack\n"
     "You are running inside a Slack bot.  Use Slack's mrkdwn dialect — "
     "single-asterisk for bold, single-tilde for strikethrough.\n"},
    {"matrix",
     "## Platform: Matrix\n"
     "You are running inside a Matrix bot.  Replies are rendered as "
     "Markdown with full HTML pass-through.\n"},
    {"whatsapp",
     "You are on a text messaging communication platform, WhatsApp. "
     "Please do not use markdown as it does not render. "
     "You can send media files natively: to deliver a file to the user, "
     "include MEDIA:/absolute/path/to/file in your response. The file "
     "will be sent as a native WhatsApp attachment — images (.jpg, .png, "
     ".webp) appear as photos, videos (.mp4, .mov) play inline, and other "
     "files arrive as downloadable documents. You can also include image "
     "URLs in markdown format ![alt](url) and they will be sent as photos."},
    {"signal",
     "You are on a text messaging communication platform, Signal. "
     "Please do not use markdown as it does not render. "
     "You can send media files natively: to deliver a file to the user, "
     "include MEDIA:/absolute/path/to/file in your response. Images "
     "(.png, .jpg, .webp) appear as photos, audio as attachments, and "
     "other files arrive as downloadable documents. You can also include "
     "image URLs in markdown format ![alt](url) and they will be sent as "
     "photos."},
    {"email",
     "You are communicating via email. Write clear, well-structured "
     "responses suitable for email. Use plain text formatting (no "
     "markdown). Keep responses concise but complete. You can send file "
     "attachments — include MEDIA:/absolute/path/to/file in your response. "
     "The subject line is preserved for threading. Do not include "
     "greetings or sign-offs unless contextually appropriate."},
    {"cron",
     "You are running as a scheduled cron job. There is no user present — "
     "you cannot ask questions, request clarification, or wait for "
     "follow-up. Execute the task fully and autonomously, making "
     "reasonable decisions where needed. Your final response is "
     "automatically delivered to the job's configured destination — put "
     "the primary content directly in your response."},
    {"sms",
     "You are communicating via SMS. Keep responses concise and use plain "
     "text only — no markdown, no formatting. SMS messages are limited to "
     "~1600 characters, so be brief and direct."},
    {"bluebubbles",
     "You are chatting via iMessage (BlueBubbles). iMessage does not "
     "render markdown formatting — use plain text. Keep responses concise "
     "as they appear as text messages. You can send media files natively: "
     "include MEDIA:/absolute/path/to/file in your response. Images "
     "(.jpg, .png, .heic) appear as photos and other files arrive as "
     "attachments."},
    {"weixin",
     "You are on Weixin/WeChat. Markdown formatting is supported, so you "
     "may use it when it improves readability, but keep the message "
     "compact and chat-friendly. You can send media files natively: "
     "include MEDIA:/absolute/path/to/file in your response. Images are "
     "sent as native photos, videos play inline when supported, and other "
     "files arrive as downloadable documents. You can also include image "
     "URLs in markdown format ![alt](url) and they will be downloaded and "
     "sent as native media when possible."},
};

}  // namespace

const std::map<std::string, std::string>& platform_hints() {
    return kPlatformHints;
}

// ──────────────────────────────────────────────────────────────────────
// Injection scanning
// ──────────────────────────────────────────────────────────────────────

bool PromptBuilder::is_injection_safe(std::string_view content) {
    static const std::regex patterns[] = {
        std::regex(R"(ignore\s+(all\s+)?previous\s+instructions)",
                   std::regex::icase),
        std::regex(R"(new\s+instructions\s*:)", std::regex::icase),
        std::regex(R"(system\s+prompt\s+override)", std::regex::icase),
        std::regex(R"(<\s*div[^>]*style\s*=\s*"?[^"]*display\s*:\s*none)",
                   std::regex::icase),
        std::regex(R"(curl\s+[^\n|]+\|\s*(sh|bash))", std::regex::icase),
        std::regex(R"((cat|grep)\s+[^\n]*\.env)", std::regex::icase),
        std::regex(R"(ssh-rsa\s+AAAA[^\s]+)"),
    };
    const std::string text(content);
    for (const auto& re : patterns) {
        if (std::regex_search(text, re)) return false;
    }
    return true;
}

std::string PromptBuilder::strip_frontmatter(std::string_view markdown) {
    // Frontmatter must START at offset 0 with "---" + newline.
    if (markdown.size() < 4) return std::string(markdown);
    if (markdown.substr(0, 3) != "---") return std::string(markdown);
    // Allow either "---\n" or "---\r\n".
    size_t after_open = 3;
    if (after_open < markdown.size() && markdown[after_open] == '\r') ++after_open;
    if (after_open >= markdown.size() || markdown[after_open] != '\n')
        return std::string(markdown);
    ++after_open;

    // Look for closing "---" on its own line.
    size_t pos = after_open;
    while (pos < markdown.size()) {
        // start of a line
        if (markdown.compare(pos, 3, "---") == 0) {
            size_t end = pos + 3;
            // Skip optional CR
            if (end < markdown.size() && markdown[end] == '\r') ++end;
            if (end == markdown.size() || markdown[end] == '\n') {
                if (end < markdown.size()) ++end;
                return std::string(markdown.substr(end));
            }
        }
        // advance to next line start
        size_t nl = markdown.find('\n', pos);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return std::string(markdown);
}

namespace {

fs::path find_git_root(const fs::path& start) {
    std::error_code ec;
    auto current = fs::weakly_canonical(start, ec);
    if (ec) current = start;
    for (auto p = current; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / ".git", ec)) return p;
        if (p == p.root_path()) break;
    }
    return {};
}

std::string read_file_safe(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

std::vector<std::string> PromptBuilder::discover_context_files(
    const fs::path& cwd) {
    std::vector<std::string> out;
    if (cwd.empty()) return out;

    std::error_code ec;
    auto start = fs::weakly_canonical(cwd, ec);
    if (ec) start = cwd;
    auto stop = find_git_root(start);

    static const char* kNames[] = {".hermes.md", "HERMES.md"};

    for (auto dir = start;; dir = dir.parent_path()) {
        for (const char* name : kNames) {
            auto candidate = dir / name;
            if (fs::is_regular_file(candidate, ec)) {
                std::string body = read_file_safe(candidate);
                if (body.empty()) continue;
                std::string stripped = strip_frontmatter(body);
                if (!is_injection_safe(stripped)) {
                    out.push_back("[BLOCKED: " + candidate.filename().string() +
                                  " contained potential prompt injection. "
                                  "Content not loaded.]");
                } else {
                    out.push_back(std::move(stripped));
                }
            }
        }
        if (!stop.empty() && dir == stop) break;
        if (dir == dir.root_path() || dir.empty() || dir == dir.parent_path()) {
            break;
        }
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────
// build_system_prompt
// ──────────────────────────────────────────────────────────────────────

std::string PromptBuilder::build_system_prompt(const PromptContext& ctx) const {
    std::string out;
    out.reserve(2048);

    // 1. Identity
    out += ctx.agent_identity.empty() ? DEFAULT_AGENT_IDENTITY
                                      : ctx.agent_identity;
    out += "\n\n";

    // 2. Platform hint
    {
        auto it = kPlatformHints.find(ctx.platform);
        if (it == kPlatformHints.end()) it = kPlatformHints.find("cli");
        if (it != kPlatformHints.end()) {
            out += it->second;
            out += '\n';
        }
    }

    // 3. CWD + timezone
    if (!ctx.cwd.empty() || !ctx.timezone.empty()) {
        out += "## Environment\n";
        if (!ctx.cwd.empty()) {
            out += "- working directory: `";
            out += ctx.cwd.string();
            out += "`\n";
        }
        if (!ctx.timezone.empty()) {
            out += "- timezone: ";
            out += ctx.timezone;
            out += '\n';
        }
        if (ctx.nous_subscription) {
            out += "- nous subscription: active\n";
        }
        out += '\n';
    }

    // 4. Memory section
    if (!ctx.memory_entries.empty() || !ctx.user_entries.empty()) {
        out += MEMORY_GUIDANCE;
        if (!ctx.memory_entries.empty()) {
            out += "\n### MEMORY.md\n";
            for (const auto& e : ctx.memory_entries) {
                out += "- ";
                out += e;
                out += '\n';
            }
        }
        if (!ctx.user_entries.empty()) {
            out += "\n### USER.md\n";
            for (const auto& e : ctx.user_entries) {
                out += "- ";
                out += e;
                out += '\n';
            }
        }
        out += '\n';
    }

    // 5. Skills index
    if (!ctx.skills_index.empty()) {
        out += SKILLS_GUIDANCE;
        out += "\n### Available skills\n";
        for (const auto& s : ctx.skills_index) {
            out += "- ";
            out += s;
            out += '\n';
        }
        out += '\n';
    }

    // 6. Context files
    if (!ctx.context_files.empty()) {
        out += "## Project context files\n";
        for (const auto& body : ctx.context_files) {
            out += body;
            if (!body.empty() && body.back() != '\n') out += '\n';
            out += '\n';
        }
    }

    // 7. Recent subdirectory hints
    if (!ctx.recent_subdirs.empty()) {
        out += "## Recently active directories\n";
        for (const auto& d : ctx.recent_subdirs) {
            out += "- ";
            out += d;
            out += '\n';
        }
        out += '\n';
    }

    // 8. Always-on guidance
    out += SESSION_SEARCH_GUIDANCE;

    // 9. Model-specific autonomy enforcement.  Without these, GPT/Codex/
    // Gemini/Gemma/Grok stop mid-task and ask the user "shall I
    // continue?" after every substantive step.  Injected last so it
    // anchors closest to the first user turn in the context window.
    if (model_needs_enforcement(ctx.model)) {
        out += '\n';
        out += TOOL_USE_ENFORCEMENT_GUIDANCE;
        if (model_is_openai_family(ctx.model)) {
            out += '\n';
            out += OPENAI_MODEL_EXECUTION_GUIDANCE;
        }
        if (model_is_google_family(ctx.model)) {
            out += '\n';
            out += GOOGLE_MODEL_OPERATIONAL_GUIDANCE;
        }
    }

    return out;
}

}  // namespace hermes::agent
