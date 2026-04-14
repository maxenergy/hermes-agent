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

namespace {

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

    return out;
}

}  // namespace hermes::agent
