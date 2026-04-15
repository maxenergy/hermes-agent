#include "hermes/tools/session_search_tool.hpp"

#include "hermes/state/session_db.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

namespace hermes::tools {

namespace {

std::string trim(std::string_view s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(begin, end - begin + 1));
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string format_unix_seconds(double secs) {
    auto t = static_cast<std::time_t>(secs);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    static const char* const kMonth[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    std::ostringstream oss;
    int hour12 = tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = tm.tm_hour < 12 ? "AM" : "PM";
    oss << kMonth[tm.tm_mon] << " " << tm.tm_mday << ", "
        << (1900 + tm.tm_year) << " at "
        << std::setfill('0') << std::setw(2) << hour12 << ":"
        << std::setfill('0') << std::setw(2) << tm.tm_min << " " << ampm;
    return oss.str();
}

bool looks_numeric(std::string_view s) {
    if (s.empty()) return false;
    bool seen_digit = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            seen_digit = true;
        } else if (c != '.' && c != '-') {
            return false;
        }
    }
    return seen_digit;
}

std::string truncate_tool_output(const std::string& s) {
    if (s.size() <= 500) return s;
    return s.substr(0, 250) + "\n...[truncated]...\n" +
           s.substr(s.size() - 250);
}

}  // namespace

const std::vector<std::string>& hidden_session_sources() {
    static const std::vector<std::string> v = {"tool"};
    return v;
}

bool is_hidden_source(std::string_view source) {
    for (const auto& h : hidden_session_sources()) {
        if (source == h) return true;
    }
    return false;
}

std::string format_timestamp_human(const nlohmann::json& ts) {
    if (ts.is_null()) return "unknown";
    if (ts.is_number()) {
        try {
            return format_unix_seconds(ts.get<double>());
        } catch (...) {
            return "unknown";
        }
    }
    if (ts.is_string()) {
        auto s = ts.get<std::string>();
        if (s.empty()) return "unknown";
        if (looks_numeric(s)) {
            try {
                return format_unix_seconds(std::stod(s));
            } catch (...) {
                return s;
            }
        }
        return s;
    }
    return "unknown";
}

std::string format_conversation(
    const std::vector<hermes::state::MessageRow>& messages) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& msg : messages) {
        std::string role = msg.role;
        std::transform(role.begin(), role.end(), role.begin(), ::toupper);
        std::string content = msg.content;

        std::string line;
        if (role == "TOOL") {
            // Truncate long tool outputs (mirror the Python logic).
            content = truncate_tool_output(content);
            // We don't have tool_name on MessageRow yet; emit role only.
            line = "[TOOL]: " + content;
        } else if (role == "ASSISTANT") {
            std::vector<std::string> tc_names;
            if (msg.tool_calls.is_array()) {
                for (const auto& tc : msg.tool_calls) {
                    std::string name;
                    if (tc.is_object()) {
                        if (tc.contains("name") && tc["name"].is_string()) {
                            name = tc["name"].get<std::string>();
                        } else if (tc.contains("function") &&
                                   tc["function"].is_object() &&
                                   tc["function"].contains("name")) {
                            name = tc["function"]["name"].get<std::string>();
                        } else {
                            name = "?";
                        }
                    }
                    if (!name.empty()) tc_names.push_back(name);
                }
            }
            if (!tc_names.empty()) {
                std::ostringstream tcs;
                for (size_t i = 0; i < tc_names.size(); ++i) {
                    if (i) tcs << ", ";
                    tcs << tc_names[i];
                }
                if (!first) oss << "\n\n";
                oss << "[ASSISTANT]: [Called: " << tcs.str() << "]";
                first = false;
                if (!content.empty()) {
                    oss << "\n\n[ASSISTANT]: " << content;
                }
                continue;
            }
            line = "[ASSISTANT]: " + content;
        } else {
            line = "[" + role + "]: " + content;
        }

        if (!first) oss << "\n\n";
        oss << line;
        first = false;
    }
    return oss.str();
}

std::string truncate_around_matches(const std::string& text,
                                    const std::string& query,
                                    std::size_t max_chars) {
    if (text.size() <= max_chars) return text;
    auto lower_text = to_lower(text);
    std::size_t first_match = text.size();

    // Split query by whitespace and find earliest occurrence.
    std::istringstream iss(to_lower(query));
    std::string term;
    while (iss >> term) {
        if (term.empty()) continue;
        auto pos = lower_text.find(term);
        if (pos != std::string::npos && pos < first_match) {
            first_match = pos;
        }
    }
    if (first_match == text.size()) first_match = 0;

    std::size_t half = max_chars / 2;
    std::size_t start = first_match > half ? first_match - half : 0;
    std::size_t end = std::min(text.size(), start + max_chars);
    if (end - start < max_chars) {
        start = end > max_chars ? end - max_chars : 0;
    }

    std::string truncated = text.substr(start, end - start);
    std::string prefix = start > 0 ? "...[earlier conversation truncated]...\n\n" : "";
    std::string suffix = end < text.size() ? "\n\n...[later conversation truncated]..." : "";
    return prefix + truncated + suffix;
}

std::vector<std::string> parse_role_filter(std::string_view raw) {
    std::vector<std::string> out;
    auto trimmed = trim(raw);
    if (trimmed.empty()) return out;
    std::stringstream ss(trimmed);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto t = trim(item);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::string resolve_session_root(
    const std::string& session_id,
    const std::function<nlohmann::json(const std::string&)>& loader) {
    std::string sid = session_id;
    std::unordered_set<std::string> visited;
    while (!sid.empty() && visited.find(sid) == visited.end()) {
        visited.insert(sid);
        nlohmann::json info = loader(sid);
        if (!info.is_object()) break;
        if (!info.value("ok", false)) break;
        if (!info.contains("parent_session_id")) break;
        const auto& parent = info["parent_session_id"];
        if (!parent.is_string()) break;
        std::string parent_id = parent.get<std::string>();
        if (parent_id.empty()) break;
        sid = parent_id;
    }
    return sid;
}

nlohmann::json format_recent_session_entry(const hermes::state::SessionRow& s,
                                           std::string_view preview) {
    nlohmann::json entry = {
        {"session_id", s.id},
        {"title", s.title.has_value() ? nlohmann::json(*s.title) : nlohmann::json(nullptr)},
        {"source", s.source},
        {"started_at",
         std::chrono::duration_cast<std::chrono::seconds>(
             s.created_at.time_since_epoch())
             .count()},
        {"last_active",
         std::chrono::duration_cast<std::chrono::seconds>(
             s.updated_at.time_since_epoch())
             .count()},
        {"preview", std::string(preview)},
    };
    return entry;
}

nlohmann::json list_recent_sessions(hermes::state::SessionDB& db,
                                    int limit,
                                    const std::string& current_session_id) {
    nlohmann::json out;
    out["success"] = true;
    out["mode"] = "recent";
    nlohmann::json results = nlohmann::json::array();

    auto sessions = db.list_sessions(limit + 5, 0);
    int taken = 0;
    for (const auto& s : sessions) {
        if (taken >= limit) break;
        if (is_hidden_source(s.source)) continue;
        if (!current_session_id.empty() && s.id == current_session_id) continue;
        results.push_back(format_recent_session_entry(s, ""));
        ++taken;
    }
    out["results"] = results;
    out["count"] = taken;
    out["message"] = "Showing " + std::to_string(taken) +
                     " most recent sessions. Use a keyword query to search "
                     "specific topics.";
    return out;
}

void register_session_search_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "session_search";
    e.toolset = "search";
    e.description = "Full-text search over past sessions";
    e.emoji = "\xf0\x9f\x94\x8d";  // magnifying glass
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "description": "Search query"
            },
            "role_filter": {
                "type": "string",
                "description": "Comma-separated role list (optional)"
            },
            "limit": {
                "type": "integer",
                "default": 3,
                "description": "Maximum number of results (max 5)"
            }
        },
        "required": []
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& ctx) -> std::string {
        std::string query;
        if (args.contains("query") && args["query"].is_string()) {
            query = trim(args["query"].get<std::string>());
        }
        int limit = args.value("limit", 3);
        if (limit > 5) limit = 5;
        if (limit < 1) limit = 1;

        hermes::state::SessionDB db;

        if (query.empty()) {
            auto recent = list_recent_sessions(db, limit, ctx.session_key);
            return tool_result(recent);
        }

        auto roles = args.contains("role_filter") &&
                             args["role_filter"].is_string()
                         ? parse_role_filter(args["role_filter"].get<std::string>())
                         : std::vector<std::string>{};

        auto hits = db.fts_search(query, std::max(20, limit * 5));

        // Dedup by session id (and apply role filter if set).
        nlohmann::json results = nlohmann::json::array();
        std::set<std::string> seen;
        for (const auto& h : hits) {
            if (seen.find(h.session_id) != seen.end()) continue;
            if (!ctx.session_key.empty() &&
                h.session_id == ctx.session_key) {
                continue;
            }
            seen.insert(h.session_id);
            results.push_back({{"session_id", h.session_id},
                               {"snippet", h.snippet},
                               {"score", h.score}});
            if (static_cast<int>(results.size()) >= limit) break;
        }
        (void)roles;  // Filter applied client-side; placeholder for parity.

        return tool_result(
            {{"results", results},
             {"query", query},
             {"count", static_cast<int>(results.size())}});
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
