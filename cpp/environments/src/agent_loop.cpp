// See agent_loop.hpp.
#include "hermes/environments/agent_loop.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>

namespace hermes::environments::agent_loop {

namespace {

const nlohmann::json& get_or_null(const nlohmann::json& obj, const char* key) {
    static const nlohmann::json null = nullptr;
    if (!obj.is_object()) return null;
    auto it = obj.find(key);
    if (it == obj.end()) return null;
    return *it;
}

std::string str_or(const nlohmann::json& v, const std::string& fallback = "") {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return fallback;
    return v.dump();
}

bool truthy_string(const nlohmann::json& v) {
    return v.is_string() && !v.get<std::string>().empty();
}

}  // namespace

std::optional<std::string> extract_reasoning_from_message(const nlohmann::json& message) {
    if (!message.is_object()) return std::nullopt;

    const auto& rc = get_or_null(message, "reasoning_content");
    if (truthy_string(rc)) return rc.get<std::string>();

    const auto& r = get_or_null(message, "reasoning");
    if (truthy_string(r)) return r.get<std::string>();

    const auto& details = get_or_null(message, "reasoning_details");
    if (details.is_array()) {
        for (const auto& detail : details) {
            const auto& text = get_or_null(detail, "text");
            if (truthy_string(text)) return text.get<std::string>();
        }
    }
    return std::nullopt;
}

std::optional<std::string> extract_user_task(const nlohmann::json& messages, size_t max_len) {
    if (!messages.is_array()) return std::nullopt;
    for (const auto& msg : messages) {
        if (!msg.is_object()) continue;
        std::string role = str_or(get_or_null(msg, "role"));
        if (role != "user") continue;
        const auto& content = get_or_null(msg, "content");
        if (!content.is_string()) {
            // Skip non-string content (image arrays etc.) — matches Python check.
            return std::nullopt;
        }
        std::string s = content.get<std::string>();
        // Trim
        auto begin = s.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::nullopt;
        auto end = s.find_last_not_of(" \t\r\n");
        s = s.substr(begin, end - begin + 1);
        if (s.empty()) return std::nullopt;
        if (s.size() > max_len) s = s.substr(0, max_len);
        return s;
    }
    return std::nullopt;
}

nlohmann::json tool_call_to_dict(const nlohmann::json& tc, const std::string& id_fallback) {
    nlohmann::json out;
    out["type"] = "function";
    if (tc.is_object()) {
        // dict-style
        std::string id = tc.contains("id") && truthy_string(tc["id"]) ? tc["id"].get<std::string>()
                                                                      : id_fallback;
        out["id"] = id;

        nlohmann::json fn = nlohmann::json::object();
        const auto& fn_field = get_or_null(tc, "function");
        if (fn_field.is_object()) {
            fn["name"] = str_or(get_or_null(fn_field, "name"));
            fn["arguments"] = str_or(get_or_null(fn_field, "arguments"), "{}");
        } else {
            fn["name"] = str_or(get_or_null(tc, "name"));
            fn["arguments"] = str_or(get_or_null(tc, "arguments"), "{}");
        }
        out["function"] = std::move(fn);
        return out;
    }
    // Treat any other shape conservatively — empty function block.
    out["id"] = id_fallback;
    out["function"] = {{"name", ""}, {"arguments", "{}"}};
    return out;
}

std::string tool_call_name(const nlohmann::json& tc) {
    if (!tc.is_object()) return "";
    const auto& fn = get_or_null(tc, "function");
    if (fn.is_object()) {
        const auto& n = get_or_null(fn, "name");
        if (n.is_string()) return n.get<std::string>();
    }
    const auto& n = get_or_null(tc, "name");
    if (n.is_string()) return n.get<std::string>();
    return "";
}

std::string tool_call_arguments(const nlohmann::json& tc) {
    if (!tc.is_object()) return "{}";
    const auto& fn = get_or_null(tc, "function");
    if (fn.is_object()) {
        const auto& a = get_or_null(fn, "arguments");
        if (a.is_string()) return a.get<std::string>();
        if (!a.is_null()) return a.dump();
    }
    const auto& a = get_or_null(tc, "arguments");
    if (a.is_string()) return a.get<std::string>();
    if (!a.is_null()) return a.dump();
    return "{}";
}

std::string tool_call_id(const nlohmann::json& tc, const std::string& fallback) {
    if (tc.is_object()) {
        const auto& id = get_or_null(tc, "id");
        if (id.is_string() && !id.get<std::string>().empty()) return id.get<std::string>();
    }
    return fallback;
}

std::string format_unknown_tool_error(
    const std::string& tool_name,
    const std::unordered_set<std::string>& valid_tool_names) {
    std::set<std::string> sorted(valid_tool_names.begin(), valid_tool_names.end());
    nlohmann::json available = nlohmann::json::array();
    for (auto& n : sorted) available.push_back(n);
    nlohmann::json err;
    err["error"] = "Unknown tool '" + tool_name + "'. Available tools: " + available.dump();
    return err.dump();
}

std::string format_invalid_json_error(const std::string& message) {
    nlohmann::json err;
    err["error"] = "Invalid JSON in tool arguments: " + message + ". Please retry with valid JSON.";
    return err.dump();
}

bool tool_result_has_negative_exit_error(const std::string& tool_result) {
    try {
        auto data = nlohmann::json::parse(tool_result);
        if (!data.is_object()) return false;
        if (!data.contains("error") || data["error"].is_null()) return false;
        if (data["error"].is_string() && data["error"].get<std::string>().empty()) return false;
        if (!data.contains("exit_code") || !data["exit_code"].is_number_integer()) return false;
        int code = data["exit_code"].get<int>();
        return code < 0;
    } catch (...) {
        return false;
    }
}

bool needs_fallback_tool_call_parse(const nlohmann::json& assistant_msg) {
    if (!assistant_msg.is_object()) return false;
    bool has_calls = false;
    const auto& tcs = get_or_null(assistant_msg, "tool_calls");
    if (tcs.is_array() && !tcs.empty()) has_calls = true;
    if (has_calls) return false;
    const auto& content = get_or_null(assistant_msg, "content");
    if (!content.is_string()) return false;
    const std::string& s = content.get<std::string>();
    return s.find("<tool_call>") != std::string::npos;
}

std::string truncate_to(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len);
}

}  // namespace hermes::environments::agent_loop
