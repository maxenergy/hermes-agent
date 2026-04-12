#include "hermes/tools/toolsets.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace hermes::tools {

namespace {

const std::vector<std::string>& core_tools_storage() {
    static const std::vector<std::string> tools = {
        // Web
        "web_search", "web_extract",
        // Terminal + process management
        "terminal", "process",
        // File manipulation
        "read_file", "write_file", "patch", "search_files",
        // Vision + image generation
        "vision_analyze", "image_generate",
        // Skills
        "skills_list", "skill_view", "skill_manage",
        // Browser automation
        "browser_navigate", "browser_snapshot", "browser_click",
        "browser_type", "browser_scroll", "browser_back",
        "browser_press", "browser_get_images",
        "browser_vision", "browser_console",
        // Text-to-speech
        "text_to_speech",
        // Planning & memory
        "todo", "memory",
        // Session history search
        "session_search",
        // Clarifying questions
        "clarify",
        // Code execution + delegation
        "execute_code", "delegate_task",
        // Cronjob management
        "cronjob",
        // Cross-platform messaging
        "send_message",
        // Home Assistant smart home control
        "ha_list_entities", "ha_get_state",
        "ha_list_services", "ha_call_service",
    };
    return tools;
}

ToolsetDef make(std::string name, std::string desc,
                std::vector<std::string> tools,
                std::vector<std::string> includes = {}) {
    return ToolsetDef{std::move(name), std::move(desc),
                      std::move(tools), std::move(includes)};
}

const std::map<std::string, ToolsetDef>& toolsets_storage() {
    static const std::map<std::string, ToolsetDef> table = [] {
        std::map<std::string, ToolsetDef> t;

        // Leaf toolsets
        t["web"] = make("web",
                        "Web research and content extraction tools",
                        {"web_search", "web_extract"});
        t["search"] = make("search",
                           "Web search only (no content extraction/scraping)",
                           {"web_search"});
        t["vision"] = make("vision",
                           "Image analysis and vision tools",
                           {"vision_analyze"});
        t["image_gen"] = make("image_gen",
                              "Creative generation tools (images)",
                              {"image_generate"});
        t["terminal"] = make("terminal",
                             "Terminal/command execution and process management",
                             {"terminal", "process"});
        t["moa"] = make("moa",
                        "Advanced reasoning and problem-solving tools",
                        {"mixture_of_agents"});
        t["skills"] = make("skills",
                           "Access, create, edit, and manage skill documents",
                           {"skills_list", "skill_view", "skill_manage"});
        t["browser"] = make(
            "browser",
            "Browser automation for web interaction "
            "(navigate, click, type, scroll, vision, console)",
            {"browser_navigate", "browser_snapshot", "browser_click",
             "browser_type", "browser_scroll", "browser_back",
             "browser_press", "browser_get_images", "browser_vision",
             "browser_console", "web_search"});
        t["file"] = make(
            "file",
            "File manipulation tools: read, write, patch, search",
            {"read_file", "write_file", "patch", "search_files"});
        t["code"] = make("code",
                         "Run scripts that call tools programmatically",
                         {"execute_code"});
        t["memory"] = make("memory",
                           "Persistent memory across sessions",
                           {"memory"});
        t["messaging"] = make("messaging",
                              "Cross-platform messaging (Telegram, Discord, ...)",
                              {"send_message"});
        t["rl"] = make(
            "rl",
            "RL training tools for Tinker-Atropos",
            {"rl_list_environments", "rl_select_environment",
             "rl_get_current_config", "rl_edit_config",
             "rl_start_training", "rl_check_status",
             "rl_stop_training", "rl_get_results",
             "rl_list_runs", "rl_test_inference"});
        t["reasoning"] = make("reasoning",
                              "Reasoning helpers — mixture of agents + clarify",
                              {"mixture_of_agents", "clarify"});

        // Composite / scenario toolsets
        t["full_stack"] = make(
            "full_stack",
            "All Hermes core tools for general-purpose CLI use",
            {},
            {"web", "vision", "image_gen", "terminal", "file", "browser",
             "skills", "memory", "code", "reasoning", "messaging"});
        t["research"] = make(
            "research",
            "Research distribution: web + browser + reasoning",
            {},
            {"web", "browser", "vision", "reasoning"});
        t["swe"] = make(
            "swe",
            "Software-engineering distribution: terminal + file + reasoning",
            {},
            {"terminal", "file", "code", "reasoning", "web"});
        t["autonomous"] = make(
            "autonomous",
            "Autonomous agent distribution: full stack minus interactive UI",
            {},
            {"web", "browser", "terminal", "file", "code", "reasoning",
             "memory", "skills"});

        return t;
    }();
    return table;
}

// ``stack`` tracks the active resolution chain (ancestors only) — a
// back-edge into the stack is a true cycle.  ``done`` tracks toolsets
// whose resolution has fully finished — re-entering one of those is a
// harmless diamond and is silently skipped.
void resolve_recursive(const std::string& name,
                       const std::map<std::string, ToolsetDef>& table,
                       std::set<std::string>& stack,
                       std::set<std::string>& done,
                       std::vector<std::string>& out_in_order) {
    if (done.count(name)) {
        return;  // diamond — already collected via another path
    }
    if (stack.count(name)) {
        throw std::invalid_argument(
            "circular toolset include detected at: " + name);
    }

    auto it = table.find(name);
    if (it == table.end()) {
        throw std::invalid_argument("unknown toolset: " + name);
    }

    stack.insert(name);
    const ToolsetDef& def = it->second;
    for (const auto& tool : def.tools) {
        out_in_order.push_back(tool);
    }
    for (const auto& inc : def.includes) {
        resolve_recursive(inc, table, stack, done, out_in_order);
    }
    stack.erase(name);
    done.insert(name);
}

std::vector<std::string> resolve_against(
    const std::string& name,
    const std::map<std::string, ToolsetDef>& table) {
    std::set<std::string> stack;
    std::set<std::string> done;
    std::vector<std::string> in_order;
    resolve_recursive(name, table, stack, done, in_order);

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(in_order.size());
    for (const auto& t : in_order) {
        if (seen.insert(t).second) out.push_back(t);
    }
    return out;
}

}  // namespace

const std::vector<std::string>& hermes_core_tools() {
    return core_tools_storage();
}

const std::map<std::string, ToolsetDef>& toolsets() {
    return toolsets_storage();
}

std::vector<std::string> resolve_toolset(const std::string& name) {
    return resolve_against(name, toolsets_storage());
}

std::vector<std::string> resolve_toolset_in_table(
    const std::string& name,
    const std::map<std::string, ToolsetDef>& table) {
    return resolve_against(name, table);
}

std::vector<std::string> resolve_multiple_toolsets(
    const std::vector<std::string>& names) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& name : names) {
        for (const auto& tool : resolve_toolset(name)) {
            if (seen.insert(tool).second) out.push_back(tool);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string validate_toolset(const std::string& name) {
    const auto& table = toolsets_storage();
    auto it = table.find(name);
    if (it == table.end()) {
        throw std::invalid_argument("unknown toolset: " + name);
    }
    return it->second.description;
}

}  // namespace hermes::tools
