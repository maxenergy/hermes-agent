#include "hermes/tools/memory_tool.hpp"

#include "hermes/state/memory_store.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::tools {

const char* const kMemoryEntryDelimiter = "\n\xc2\xa7\n";  // "\n§\n"

namespace {

struct ThreatPattern {
    const char* regex;
    const char* id;
};

// Mirror of _MEMORY_THREAT_PATTERNS in tools/memory_tool.py.
const std::array<ThreatPattern, 12>& threat_patterns() {
    static const std::array<ThreatPattern, 12> patterns = {{
        {R"(ignore\s+(previous|all|above|prior)\s+instructions)", "prompt_injection"},
        {R"(you\s+are\s+now\s+)", "role_hijack"},
        {R"(do\s+not\s+tell\s+the\s+user)", "deception_hide"},
        {R"(system\s+prompt\s+override)", "sys_prompt_override"},
        {R"(disregard\s+(your|all|any)\s+(instructions|rules|guidelines))", "disregard_rules"},
        {R"(act\s+as\s+(if|though)\s+you\s+(have\s+no|don't\s+have)\s+(restrictions|limits|rules))", "bypass_restrictions"},
        {R"(curl\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))", "exfil_curl"},
        {R"(wget\s+[^\n]*\$\{?\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API))", "exfil_wget"},
        {R"(cat\s+[^\n]*(\.env|credentials|\.netrc|\.pgpass|\.npmrc|\.pypirc))", "read_secrets"},
        {R"(authorized_keys)", "ssh_backdoor"},
        {R"((\$HOME|~)/\.ssh)", "ssh_access"},
        {R"((\$HOME|~)/\.hermes/\.env)", "hermes_env"},
    }};
    return patterns;
}

const std::unordered_set<std::string>& invisible_chars() {
    // UTF-8 encoded forms of the markers in _INVISIBLE_CHARS.
    static const std::unordered_set<std::string> v = {
        "\xe2\x80\x8b",  // U+200B ZERO WIDTH SPACE
        "\xe2\x80\x8c",  // U+200C ZERO WIDTH NON-JOINER
        "\xe2\x80\x8d",  // U+200D ZERO WIDTH JOINER
        "\xe2\x81\xa0",  // U+2060 WORD JOINER
        "\xef\xbb\xbf",  // U+FEFF BOM
        "\xe2\x80\xaa",  // U+202A LEFT-TO-RIGHT EMBEDDING
        "\xe2\x80\xab",  // U+202B RIGHT-TO-LEFT EMBEDDING
        "\xe2\x80\xac",  // U+202C POP DIRECTIONAL FORMATTING
        "\xe2\x80\xad",  // U+202D LEFT-TO-RIGHT OVERRIDE
        "\xe2\x80\xae",  // U+202E RIGHT-TO-LEFT OVERRIDE
    };
    return v;
}

std::string thousands_separator(std::size_t n) {
    auto s = std::to_string(n);
    std::string out;
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count > 0 && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

hermes::state::MemoryFile parse_memory_file(const nlohmann::json& args) {
    std::string file = "agent";
    if (args.contains("file") && args["file"].is_string()) {
        file = args["file"].get<std::string>();
    }
    if (file == "user") return hermes::state::MemoryFile::User;
    return hermes::state::MemoryFile::Agent;
}

std::size_t char_limit_for(hermes::state::MemoryFile which) {
    return which == hermes::state::MemoryFile::User ? kMemoryUserCharLimit
                                                    : kMemoryAgentCharLimit;
}

std::string join_entries(const std::vector<std::string>& entries) {
    if (entries.empty()) return {};
    std::ostringstream oss;
    bool first = true;
    for (const auto& e : entries) {
        if (!first) oss << kMemoryEntryDelimiter;
        oss << e;
        first = false;
    }
    return oss.str();
}

std::string sanitize_entry(std::string_view raw) {
    // Trim ASCII whitespace from both ends.
    auto begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = raw.find_last_not_of(" \t\r\n");
    auto trimmed = raw.substr(begin, end - begin + 1);

    // Collapse runs of \n§\n inside the entry — they would otherwise
    // be parsed as multiple entries on the next read.
    std::string out;
    out.reserve(trimmed.size());
    std::string sentinel = "\n\xc2\xa7\n";
    std::size_t i = 0;
    while (i < trimmed.size()) {
        if (i + sentinel.size() <= trimmed.size() &&
            std::memcmp(trimmed.data() + i, sentinel.data(),
                        sentinel.size()) == 0) {
            out.push_back('\n');
            i += sentinel.size();
        } else {
            out.push_back(trimmed[i]);
            ++i;
        }
    }
    return out;
}

bool contains_invisible_unicode(std::string_view content) {
    for (const auto& marker : invisible_chars()) {
        if (content.find(marker) != std::string_view::npos) return true;
    }
    return false;
}

std::optional<std::string> scan_memory_content(std::string_view content) {
    if (contains_invisible_unicode(content)) {
        return std::string("invisible_unicode");
    }
    std::string body(content);
    for (const auto& tp : threat_patterns()) {
        try {
            std::regex re(tp.regex,
                          std::regex::ECMAScript | std::regex::icase);
            if (std::regex_search(body, re)) {
                return std::string(tp.id);
            }
        } catch (const std::regex_error&) {
            // Pattern compilation failures should not block users — log
            // and continue.  In the C++ port we simply skip.
            continue;
        }
    }
    return std::nullopt;
}

std::string format_usage(std::size_t used, std::size_t limit) {
    return thousands_separator(used) + "/" + thousands_separator(limit);
}

std::vector<std::size_t> find_matching_indexes(
    const std::vector<std::string>& entries, std::string_view needle) {
    std::vector<std::size_t> out;
    if (needle.empty()) return out;
    std::string n(needle);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].find(n) != std::string::npos) out.push_back(i);
    }
    return out;
}

nlohmann::json build_add_response(hermes::state::MemoryFile which,
                                  const std::vector<std::string>& entries,
                                  std::string_view note) {
    auto used = join_entries(entries).size();
    auto limit = char_limit_for(which);
    return nlohmann::json{
        {"added", true},
        {"count", static_cast<int>(entries.size())},
        {"usage", format_usage(used, limit)},
        {"note", std::string(note)},
    };
}

namespace {

std::string entry_preview(const std::string& s) {
    if (s.size() <= 80) return s;
    return s.substr(0, 80) + "...";
}

}  // namespace

void register_memory_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "memory";
    e.toolset = "memory";
    e.description = "Manage persistent memory entries (add/read/replace/remove)";
    e.emoji = "\xf0\x9f\xa7\xa0";  // brain emoji
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["add", "read", "replace", "remove"],
                "description": "The memory operation to perform"
            },
            "file": {
                "type": "string",
                "enum": ["agent", "user"],
                "default": "agent",
                "description": "Which memory file to operate on"
            },
            "entry": {
                "type": "string",
                "description": "The entry text (for add)"
            },
            "needle": {
                "type": "string",
                "description": "Substring to match (for replace/remove)"
            },
            "replacement": {
                "type": "string",
                "description": "Replacement text (for replace)"
            }
        },
        "required": ["action"]
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& /*ctx*/) -> std::string {
        if (!args.contains("action") || !args["action"].is_string()) {
            return tool_error("missing required parameter: action");
        }
        const auto action = args["action"].get<std::string>();
        auto which = parse_memory_file(args);
        hermes::state::MemoryStore store;
        const auto limit = char_limit_for(which);

        if (action == "add") {
            if (!args.contains("entry") || !args["entry"].is_string()) {
                return tool_error("add requires 'entry' parameter");
            }
            auto raw = args["entry"].get<std::string>();
            auto entry = sanitize_entry(raw);
            if (entry.empty()) {
                return tool_error("Content cannot be empty.");
            }
            if (auto threat = scan_memory_content(entry); threat) {
                return tool_error("Blocked: content matches threat pattern '" +
                                  *threat + "'");
            }
            auto current = store.read_all(which);
            // Reject exact duplicates (no-op success)
            if (std::find(current.begin(), current.end(), entry) !=
                current.end()) {
                return tool_result(build_add_response(
                    which, current, "Entry already exists (no duplicate added)."));
            }
            auto candidate = current;
            candidate.push_back(entry);
            auto new_total = join_entries(candidate).size();
            if (new_total > limit) {
                return tool_error(
                    "Memory at " + format_usage(join_entries(current).size(), limit) +
                        " chars. Adding this entry (" +
                        std::to_string(entry.size()) +
                        " chars) would exceed the limit.",
                    {{"usage", format_usage(join_entries(current).size(), limit)}});
            }
            store.add(which, entry);
            auto entries = store.read_all(which);
            return tool_result(
                build_add_response(which, entries, "Entry added."));
        }

        if (action == "read") {
            auto entries = store.read_all(which);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& entry : entries) arr.push_back(entry);
            return tool_result({{"entries", arr},
                                {"count", static_cast<int>(entries.size())},
                                {"usage", format_usage(join_entries(entries).size(),
                                                       limit)}});
        }

        if (action == "replace") {
            if (!args.contains("needle") || !args["needle"].is_string()) {
                return tool_error("replace requires 'needle' parameter");
            }
            if (!args.contains("replacement") ||
                !args["replacement"].is_string()) {
                return tool_error("replace requires 'replacement' parameter");
            }
            auto needle = args["needle"].get<std::string>();
            auto replacement = sanitize_entry(args["replacement"].get<std::string>());
            if (replacement.empty()) {
                return tool_error(
                    "new_content cannot be empty. Use 'remove' to delete entries.");
            }
            if (auto threat = scan_memory_content(replacement); threat) {
                return tool_error(
                    "Blocked: replacement matches threat pattern '" +
                    *threat + "'");
            }
            auto entries = store.read_all(which);
            auto idxs = find_matching_indexes(entries, needle);
            if (idxs.empty()) {
                return tool_error("No entry matched '" + needle + "'.");
            }
            // Multi-match disambiguation
            if (idxs.size() > 1) {
                std::unordered_set<std::string> uniq;
                for (auto i : idxs) uniq.insert(entries[i]);
                if (uniq.size() > 1) {
                    nlohmann::json previews = nlohmann::json::array();
                    for (auto i : idxs) previews.push_back(entry_preview(entries[i]));
                    return tool_error("Multiple entries matched '" + needle +
                                          "'. Be more specific.",
                                      {{"matches", previews}});
                }
            }
            auto candidate = entries;
            candidate[idxs[0]] = replacement;
            if (join_entries(candidate).size() > limit) {
                return tool_error(
                    "Replacement would put memory at " +
                    format_usage(join_entries(candidate).size(), limit) +
                    " chars.");
            }
            store.replace(which, needle, replacement);
            return tool_result({{"replaced", true},
                                {"usage", format_usage(
                                              join_entries(candidate).size(), limit)}});
        }

        if (action == "remove") {
            if (!args.contains("needle") || !args["needle"].is_string()) {
                return tool_error("remove requires 'needle' parameter");
            }
            auto needle = args["needle"].get<std::string>();
            auto entries = store.read_all(which);
            auto idxs = find_matching_indexes(entries, needle);
            if (idxs.empty()) {
                return tool_error("No entry matched '" + needle + "'.");
            }
            store.remove(which, needle);
            auto post = store.read_all(which);
            return tool_result(
                {{"removed", true},
                 {"count", static_cast<int>(post.size())},
                 {"usage", format_usage(join_entries(post).size(), limit)}});
        }

        return tool_error("invalid action: " + action +
                          "; expected add|read|replace|remove");
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
