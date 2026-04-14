// C++17 port of guidance constants + context scanner from
// agent/prompt_builder.py.
#include "hermes/agent/prompt_guidance.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace hermes::agent::guidance {

const std::string TOOL_USE_ENFORCEMENT =
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
    "only describe intentions without acting are not acceptable.";

const std::string OPENAI_MODEL_EXECUTION =
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
    "</tool_persistence>\n\n"
    "<mandatory_tool_use>\n"
    "NEVER answer these from memory or mental computation — ALWAYS use a "
    "tool:\n"
    "- Arithmetic, math, calculations → use terminal or execute_code\n"
    "- Hashes, encodings, checksums → use terminal (e.g. sha256sum, "
    "base64)\n"
    "- Current time, date, timezone → use terminal (e.g. date)\n"
    "- System state: OS, CPU, memory, disk, ports, processes → use "
    "terminal\n"
    "- File contents, sizes, line counts → use read_file, search_files, "
    "or terminal\n"
    "- Git history, branches, diffs → use terminal\n"
    "- Current facts (weather, news, versions) → use web_search\n"
    "Your memory and user profile describe the USER, not the system you "
    "are running on. The execution environment may differ from what the "
    "user profile says about their personal setup.\n"
    "</mandatory_tool_use>\n\n"
    "<act_dont_ask>\n"
    "When a question has an obvious default interpretation, act on it "
    "immediately instead of asking for clarification. Examples:\n"
    "- 'Is port 443 open?' → check THIS machine (don't ask 'open "
    "where?')\n"
    "- 'What OS am I running?' → check the live system (don't use user "
    "profile)\n"
    "- 'What time is it?' → run `date` (don't guess)\n"
    "Only ask for clarification when the ambiguity genuinely changes "
    "what tool you would call.\n"
    "</act_dont_ask>\n\n"
    "<prerequisite_checks>\n"
    "- Before taking an action, check whether prerequisite discovery, "
    "lookup, or context-gathering steps are needed.\n"
    "- Do not skip prerequisite steps just because the final action "
    "seems obvious.\n"
    "- If a task depends on output from a prior step, resolve that "
    "dependency first.\n"
    "</prerequisite_checks>\n\n"
    "<verification>\n"
    "Before finalizing your response:\n"
    "- Correctness: does the output satisfy every stated requirement?\n"
    "- Grounding: are factual claims backed by tool outputs or provided "
    "context?\n"
    "- Formatting: does the output match the requested format or "
    "schema?\n"
    "- Safety: if the next step has side effects (file writes, commands, "
    "API calls), confirm scope before executing.\n"
    "</verification>\n\n"
    "<missing_context>\n"
    "- If required context is missing, do NOT guess or hallucinate an "
    "answer.\n"
    "- Use the appropriate lookup tool when missing information is "
    "retrievable (search_files, web_search, read_file, etc.).\n"
    "- Ask a clarifying question only when the information cannot be "
    "retrieved by tools.\n"
    "- If you must proceed with incomplete information, label "
    "assumptions explicitly.\n"
    "</missing_context>";

const std::string GOOGLE_MODEL_OPERATIONAL =
    "# Google model operational directives\n"
    "Follow these operational rules strictly:\n"
    "- **Absolute paths:** Always construct and use absolute file paths "
    "for all file system operations. Combine the project root with "
    "relative paths.\n"
    "- **Verify first:** Use read_file/search_files to check file "
    "contents and project structure before making changes. Never guess "
    "at file contents.\n"
    "- **Dependency checks:** Never assume a library is available. Check "
    "package.json, requirements.txt, Cargo.toml, etc. before importing.\n"
    "- **Conciseness:** Keep explanatory text brief — a few sentences, "
    "not paragraphs. Focus on actions and results over narration.\n"
    "- **Parallel tool calls:** When you need to perform multiple "
    "independent operations (e.g. reading several files), make all the "
    "tool calls in a single response rather than sequentially.\n"
    "- **Non-interactive commands:** Use flags like -y, --yes, "
    "--non-interactive to prevent CLI tools from hanging on prompts.\n"
    "- **Keep going:** Work autonomously until the task is fully "
    "resolved. Don't stop with a plan — execute it.\n";

namespace {

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains_any(const std::string& lowered,
                  std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (lowered.find(n) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

bool model_needs_tool_use_enforcement(const std::string& model) {
    const std::string low = lower_copy(model);
    return contains_any(low, {"gpt", "codex", "gemini", "gemma", "grok"});
}

bool model_is_openai(const std::string& model) {
    const std::string low = lower_copy(model);
    return contains_any(low, {"gpt", "codex"});
}

bool model_is_google(const std::string& model) {
    const std::string low = lower_copy(model);
    return contains_any(low, {"gemini", "gemma"});
}

bool model_uses_developer_role(const std::string& model) {
    const std::string low = lower_copy(model);
    return contains_any(low, {"gpt-5", "codex"});
}

std::string select_guidance_for_model(const std::string& model) {
    std::string out;
    if (model_needs_tool_use_enforcement(model)) {
        out += TOOL_USE_ENFORCEMENT;
    }
    if (model_is_openai(model)) {
        if (!out.empty()) out += "\n\n";
        out += OPENAI_MODEL_EXECUTION;
    }
    if (model_is_google(model)) {
        if (!out.empty()) out += "\n\n";
        out += GOOGLE_MODEL_OPERATIONAL;
    }
    return out;
}

}  // namespace hermes::agent::guidance

// ── Context scanner ────────────────────────────────────────────────────

namespace hermes::agent::context_scanner {

namespace {

struct Pattern {
    const char* regex;
    const char* id;
};

const Pattern k_patterns[] = {
    {R"(ignore\s+(previous|all|above|prior)\s+instructions)", "prompt_injection"},
    {R"(do\s+not\s+tell\s+the\s+user)", "deception_hide"},
    {R"(system\s+prompt\s+override)", "sys_prompt_override"},
    {R"(disregard\s+(your|all|any)\s+(instructions|rules|guidelines))",
     "disregard_rules"},
    {R"(act\s+as\s+(if|though)\s+you\s+(have\s+no|don't\s+have)\s+(restrictions|limits|rules))",
     "bypass_restrictions"},
    {R"(<!--[^>]*(?:ignore|override|system|secret|hidden)[^>]*-->)",
     "html_comment_injection"},
    {R"(<\s*div\s+style\s*=\s*["'][\s\S]*?display\s*:\s*none)", "hidden_div"},
    {R"(translate\s+.*\s+into\s+.*\s+and\s+(execute|run|eval))",
     "translate_execute"},
    {R"(curl\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))",
     "exfil_curl"},
    {R"(cat\s+[^\n]*(\.env|credentials|\.netrc|\.pgpass))", "read_secrets"},
};

// Invisible Unicode code points that get stripped / detected.
const char* k_invisible_sequences[] = {
    "\xe2\x80\x8b",  // U+200B ZERO WIDTH SPACE
    "\xe2\x80\x8c",  // U+200C ZERO WIDTH NON-JOINER
    "\xe2\x80\x8d",  // U+200D ZERO WIDTH JOINER
    "\xe2\x81\xa0",  // U+2060 WORD JOINER
    "\xef\xbb\xbf",  // U+FEFF BYTE ORDER MARK
    "\xe2\x80\xaa",  // U+202A LEFT-TO-RIGHT EMBEDDING
    "\xe2\x80\xab",  // U+202B RIGHT-TO-LEFT EMBEDDING
    "\xe2\x80\xac",  // U+202C POP DIRECTIONAL FORMATTING
    "\xe2\x80\xad",  // U+202D LEFT-TO-RIGHT OVERRIDE
    "\xe2\x80\xae",  // U+202E RIGHT-TO-LEFT OVERRIDE
};
const char* k_invisible_labels[] = {
    "invisible unicode U+200B",
    "invisible unicode U+200C",
    "invisible unicode U+200D",
    "invisible unicode U+2060",
    "invisible unicode U+FEFF",
    "invisible unicode U+202A",
    "invisible unicode U+202B",
    "invisible unicode U+202C",
    "invisible unicode U+202D",
    "invisible unicode U+202E",
};

}  // namespace

ScanResult scan_context_content(const std::string& content,
                                const std::string& filename) {
    ScanResult r;
    std::vector<std::string> findings;

    const std::size_t n = sizeof(k_invisible_sequences) / sizeof(k_invisible_sequences[0]);
    for (std::size_t i = 0; i < n; ++i) {
        if (content.find(k_invisible_sequences[i]) != std::string::npos) {
            findings.emplace_back(k_invisible_labels[i]);
        }
    }

    for (const auto& p : k_patterns) {
        std::regex re(p.regex, std::regex::icase);
        if (std::regex_search(content, re)) {
            findings.emplace_back(p.id);
        }
    }

    if (findings.empty()) {
        r.output = content;
        return r;
    }

    std::string joined;
    for (std::size_t i = 0; i < findings.size(); ++i) {
        if (i) joined += ", ";
        joined += findings[i];
    }
    r.blocked = true;
    r.reason = joined;
    r.output = "[BLOCKED: " + filename + " contained potential prompt "
               "injection (" + joined + "). Content not loaded.]";
    return r;
}

}  // namespace hermes::agent::context_scanner
