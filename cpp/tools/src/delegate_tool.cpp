#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <future>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

AgentFactory g_agent_factory;

std::string handle_delegate_task(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    if (!g_agent_factory) {
        return tool_error("delegate requires agent factory — call set_delegate_factory() at startup");
    }

    const auto goal = args.at("goal").get<std::string>();
    const auto constraints =
        args.contains("constraints")
            ? args["constraints"].get<std::string>()
            : std::string{};
    const auto model =
        args.contains("model") ? args["model"].get<std::string>()
                               : std::string{};

    // Save and restore last_resolved_tool_names around sub-agent execution
    // (per AGENTS.md pitfall: process-global state can be stale during child).
    auto saved = ToolRegistry::instance().last_resolved_tool_names();

    auto agent = g_agent_factory(model);
    if (!agent) {
        ToolRegistry::instance().set_last_resolved_tool_names(saved);
        return tool_error("agent factory returned nullptr for model: " +
                          (model.empty() ? "(default)" : model));
    }

    // Build sub-agent prompt from goal + constraints.
    std::string prompt = "Goal: " + goal;
    if (!constraints.empty()) {
        prompt += "\nConstraints: " + constraints;
    }

    auto output = agent->run(prompt, constraints);

    // Restore parent's tool name state.
    ToolRegistry::instance().set_last_resolved_tool_names(saved);

    nlohmann::json r;
    r["response"] = output;
    r["completed"] = true;
    return tool_result(r);
}

std::string handle_mixture_of_agents(const nlohmann::json& args,
                                     const ToolContext& /*ctx*/) {
    if (!g_agent_factory) {
        return tool_error("mixture_of_agents requires agent factory — call set_delegate_factory() at startup");
    }

    const auto prompt = args.at("prompt").get<std::string>();
    const auto& models_arr = args.at("models");
    if (!models_arr.is_array() || models_arr.empty()) {
        return tool_error("models must be a non-empty array of model names");
    }

    std::vector<std::string> models;
    for (const auto& m : models_arr) {
        models.push_back(m.get<std::string>());
    }

    // Save tool name state before spawning sub-agents.
    auto saved = ToolRegistry::instance().last_resolved_tool_names();

    // Launch all sub-agents in parallel using std::async.
    std::vector<std::future<std::string>> futures;
    futures.reserve(models.size());
    for (const auto& model : models) {
        futures.push_back(std::async(std::launch::async,
            [&prompt, &model]() -> std::string {
                auto agent = g_agent_factory(model);
                if (!agent) return "(error: factory returned nullptr for " + model + ")";
                return agent->run(prompt, "");
            }));
    }

    // Collect results.
    nlohmann::json responses = nlohmann::json::array();
    for (size_t i = 0; i < futures.size(); ++i) {
        nlohmann::json entry;
        entry["model"] = models[i];
        try {
            entry["response"] = futures[i].get();
        } catch (const std::exception& ex) {
            entry["response"] = std::string("(error: ") + ex.what() + ")";
        }
        responses.push_back(std::move(entry));
    }

    // Restore parent's tool name state.
    ToolRegistry::instance().set_last_resolved_tool_names(saved);

    nlohmann::json r;
    r["responses"] = responses;
    r["model_count"] = static_cast<int>(models.size());
    return tool_result(r);
}

}  // namespace

void register_delegate_tools(AgentFactory factory) {
    g_agent_factory = std::move(factory);
    auto& reg = ToolRegistry::instance();

    {
        ToolEntry e;
        e.name = "delegate_task";
        e.toolset = "moa";
        e.description = "Delegate a task to a sub-agent";
        e.emoji = "\xF0\x9F\xA4\x96";  // robot
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"goal",
               {{"type", "string"}, {"description", "Task goal"}}},
              {"constraints",
               {{"type", "string"},
                {"description", "Optional constraints"}}},
              {"model",
               {{"type", "string"},
                {"description", "Optional model override"}}}}},
            {"required", nlohmann::json::array({"goal"})}};
        e.handler = handle_delegate_task;
        reg.register_tool(std::move(e));
    }

    {
        ToolEntry e;
        e.name = "mixture_of_agents";
        e.toolset = "moa";
        e.description = "Call multiple LLMs in parallel and aggregate";
        e.emoji = "\xF0\x9F\x94\x80";  // shuffle
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"prompt",
               {{"type", "string"}, {"description", "Prompt to send"}}},
              {"models",
               {{"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "List of model names"}}}}},
            {"required", nlohmann::json::array({"prompt", "models"})}};
        e.handler = handle_mixture_of_agents;
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
