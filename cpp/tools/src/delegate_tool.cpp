#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <string>

namespace hermes::tools {

namespace {

AgentFactory g_agent_factory;

std::string handle_delegate_task(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    if (!g_agent_factory) {
        return tool_error("delegate not yet wired");
    }

    const auto goal = args.at("goal").get<std::string>();
    const auto constraints =
        args.contains("constraints")
            ? args["constraints"].get<std::string>()
            : std::string{};
    const auto model =
        args.contains("model") ? args["model"].get<std::string>()
                               : std::string{};

    auto agent = g_agent_factory(model);
    if (!agent) {
        return tool_error("agent factory returned nullptr");
    }

    auto output = agent->run(goal, constraints);
    nlohmann::json r;
    r["output"] = output;
    return tool_result(r);
}

std::string handle_mixture_of_agents(const nlohmann::json& args,
                                     const ToolContext& /*ctx*/) {
    (void)args;
    return tool_error("mixture_of_agents not yet wired");
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
