// Depth port of agent/prompt_builder.py helpers.

#include "hermes/agent/prompt_builder_depth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::agent::prompt_depth {

namespace {

// Threat pattern table — mirror of _CONTEXT_THREAT_PATTERNS in Python.
struct Pattern {
    const char* regex;
    const char* id;
};

const std::vector<Pattern>& threat_patterns() {
    static const std::vector<Pattern> kPatterns = {
        {R"(ignore\s+(previous|all|above|prior)\s+instructions)", "prompt_injection"},
        {R"(do\s+not\s+tell\s+the\s+user)", "deception_hide"},
        {R"(system\s+prompt\s+override)", "sys_prompt_override"},
        {R"(disregard\s+(your|all|any)\s+(instructions|rules|guidelines))", "disregard_rules"},
        {R"(act\s+as\s+(if|though)\s+you\s+(have\s+no|don't\s+have)\s+(restrictions|limits|rules))", "bypass_restrictions"},
        {R"(<!--[^>]*(?:ignore|override|system|secret|hidden)[^>]*-->)", "html_comment_injection"},
        {R"(<\s*div\s+style\s*=\s*["'][\s\S]*?display\s*:\s*none)", "hidden_div"},
        {R"(translate\s+.*\s+into\s+.*\s+and\s+(execute|run|eval))", "translate_execute"},
        {R"(curl\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))", "exfil_curl"},
        {R"(cat\s+[^\n]*(\.env|credentials|\.netrc|\.pgpass))", "read_secrets"},
    };
    return kPatterns;
}

// Invisible unicode set from _CONTEXT_INVISIBLE_CHARS.  Stored as the
// UTF-8 encoded form so we can do a substring match on raw input.
struct InvisibleChar {
    std::uint32_t code_point;
    const char* utf8;
};
const std::vector<InvisibleChar>& invisible_chars() {
    static const std::vector<InvisibleChar> kChars = {
        {0x200B, "\xE2\x80\x8B"},
        {0x200C, "\xE2\x80\x8C"},
        {0x200D, "\xE2\x80\x8D"},
        {0x2060, "\xE2\x81\xA0"},
        {0xFEFF, "\xEF\xBB\xBF"},
        {0x202A, "\xE2\x80\xAA"},
        {0x202B, "\xE2\x80\xAB"},
        {0x202C, "\xE2\x80\xAC"},
        {0x202D, "\xE2\x80\xAD"},
        {0x202E, "\xE2\x80\xAE"},
    };
    return kChars;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string format_invisible_marker(std::uint32_t cp) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "invisible unicode U+%04X", cp);
    return std::string(buf);
}

std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

}  // namespace

std::vector<std::string> threat_pattern_ids() {
    std::vector<std::string> ids;
    ids.reserve(threat_patterns().size());
    for (const auto& p : threat_patterns()) ids.emplace_back(p.id);
    return ids;
}

ContextScanResult scan_context_content(std::string_view content,
                                       std::string_view filename) {
    ContextScanResult r;
    // Invisible unicode — raw UTF-8 substring search.
    for (const auto& ic : invisible_chars()) {
        if (content.find(ic.utf8) != std::string_view::npos) {
            r.findings.push_back(format_invisible_marker(ic.code_point));
        }
    }
    // Threat patterns — case insensitive.
    std::string input(content);
    for (const auto& p : threat_patterns()) {
        try {
            std::regex re(p.regex, std::regex::icase);
            if (std::regex_search(input, re)) {
                r.findings.emplace_back(p.id);
            }
        } catch (const std::regex_error&) {
            // If a pattern is malformed on this STL, skip it rather than crash.
        }
    }
    if (!r.findings.empty()) {
        r.blocked = true;
        std::ostringstream oss;
        oss << "[BLOCKED: " << filename << " contained potential prompt injection (";
        for (std::size_t i = 0; i < r.findings.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << r.findings[i];
        }
        oss << "). Content not loaded.]";
        r.output = oss.str();
    } else {
        r.output = std::string(content);
    }
    return r;
}

std::string strip_yaml_frontmatter(std::string_view content) {
    if (!starts_with(content, "---")) return std::string(content);
    // Python: content.find("\n---", 3)
    auto pos = content.find("\n---", 3);
    if (pos == std::string_view::npos) return std::string(content);
    // Skip past "\n---" and any trailing newline.
    std::size_t after = pos + 4;
    // "lstrip('\n')"
    while (after < content.size() && content[after] == '\n') ++after;
    if (after >= content.size()) return std::string(content);
    return std::string(content.substr(after));
}

std::string truncate_content(std::string_view content,
                             std::string_view filename,
                             std::size_t max_chars) {
    if (content.size() <= max_chars) return std::string(content);
    std::size_t head_chars = static_cast<std::size_t>(
        std::floor(static_cast<double>(max_chars) * kContextTruncateHeadRatio));
    std::size_t tail_chars = static_cast<std::size_t>(
        std::floor(static_cast<double>(max_chars) * kContextTruncateTailRatio));
    std::ostringstream oss;
    oss << content.substr(0, head_chars);
    oss << "\n\n[...truncated " << filename
        << ": kept " << head_chars << "+" << tail_chars
        << " of " << content.size()
        << " chars. Use file tools to read the full file.]\n\n";
    oss << content.substr(content.size() - tail_chars);
    return oss.str();
}

bool skill_should_show(
    const SkillConditions& conds,
    const std::optional<std::unordered_set<std::string>>& available_tools,
    const std::optional<std::unordered_set<std::string>>& available_toolsets) {
    if (!available_tools.has_value() && !available_toolsets.has_value()) {
        return true;
    }
    const auto& at = available_tools.has_value()
        ? *available_tools : std::unordered_set<std::string>();
    const auto& ats = available_toolsets.has_value()
        ? *available_toolsets : std::unordered_set<std::string>();

    for (const auto& ts : conds.fallback_for_toolsets) {
        if (ats.count(ts) != 0) return false;
    }
    for (const auto& t : conds.fallback_for_tools) {
        if (at.count(t) != 0) return false;
    }
    for (const auto& ts : conds.requires_toolsets) {
        if (ats.count(ts) == 0) return false;
    }
    for (const auto& t : conds.requires_tools) {
        if (at.count(t) == 0) return false;
    }
    return true;
}

SkillPathParts parse_skill_path_parts(std::string_view relative_path,
                                      std::string_view parent_dir_name) {
    SkillPathParts out;
    auto parts = split(relative_path, '/');
    // Remove empty segments (double slashes).
    parts.erase(std::remove_if(parts.begin(), parts.end(),
                               [](const std::string& s) { return s.empty(); }),
                parts.end());
    if (parts.size() >= 2) {
        out.skill_name = parts[parts.size() - 2];
        if (parts.size() > 2) {
            std::ostringstream oss;
            for (std::size_t i = 0; i + 2 < parts.size(); ++i) {
                if (i > 0) oss << "/";
                oss << parts[i];
            }
            out.category = oss.str();
        } else {
            out.category = parts[0];
        }
    } else {
        out.category = "general";
        out.skill_name = std::string(parent_dir_name);
    }
    return out;
}

std::string render_project_context_section(std::string_view filename,
                                           std::string_view body) {
    std::string out;
    out.reserve(body.size() + filename.size() + 8);
    out += "## ";
    out.append(filename);
    out += "\n\n";
    out.append(body);
    return out;
}

bool needs_tool_use_enforcement(std::string_view model_id) {
    static const std::vector<std::string> kFamilies = {
        "gpt", "codex", "gemini", "gemma", "grok",
    };
    std::string lower = to_lower(model_id);
    for (const auto& fam : kFamilies) {
        if (lower.find(fam) != std::string::npos) return true;
    }
    return false;
}

bool is_openai_execution_family(std::string_view model_id) {
    std::string lower = to_lower(model_id);
    return lower.find("gpt") != std::string::npos
        || lower.find("codex") != std::string::npos;
}

}  // namespace hermes::agent::prompt_depth
