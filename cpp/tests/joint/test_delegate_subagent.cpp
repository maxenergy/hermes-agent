// Joint integration tests — delegate_task / mixture_of_agents + ToolRegistry.

#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

using hermes::tools::AgentFactory;
using hermes::tools::AIAgent;
using hermes::tools::ToolContext;
using hermes::tools::ToolRegistry;
using hermes::tools::register_delegate_tools;
using json = nlohmann::json;

namespace {

struct Recorder {
    std::vector<std::string> goals;
    std::vector<std::string> models;
};

AgentFactory make_recording_factory(Recorder* rec) {
    return [rec](const std::string& model) -> std::unique_ptr<AIAgent> {
        struct SubAgent : public AIAgent {
            Recorder* rec;
            std::string model;
            SubAgent(Recorder* r, std::string m) : rec(r), model(std::move(m)) {}
            std::string run(const std::string& goal,
                            const std::string& /*constraints*/) override {
                rec->goals.push_back(goal);
                rec->models.push_back(model);
                return "sub[" + model + "]:" + goal;
            }
        };
        return std::make_unique<SubAgent>(rec, model.empty() ? "default" : model);
    };
}

}  // namespace

// 1. register_delegate_tools with fake factory — dispatch succeeds and output
// is embedded in the tool result.
TEST(JointDelegate, DelegateDispatchReturnsChildOutput) {
    ToolRegistry::instance().clear();
    Recorder rec;
    register_delegate_tools(make_recording_factory(&rec));

    auto out = ToolRegistry::instance().dispatch(
        "delegate_task",
        json{{"goal", "summarise the docs"},
             {"constraints", "be brief"}},
        ToolContext{});
    auto parsed = json::parse(out);
    EXPECT_FALSE(parsed.contains("error"));
    ASSERT_TRUE(parsed.contains("results"));
    ASSERT_EQ(parsed["results"].size(), 1u);
    const auto& r0 = parsed["results"][0];
    EXPECT_EQ(r0["status"].get<std::string>(), "completed");
    EXPECT_NE(r0["summary"].get<std::string>().find("summarise the docs"),
              std::string::npos);

    ASSERT_EQ(rec.goals.size(), 1u);
    // run_with_context legacy fallback prefixes goal with "Goal: ".
    EXPECT_NE(rec.goals[0].find("summarise the docs"), std::string::npos);

    ToolRegistry::instance().clear();
}

// 2. Mixture-of-Agents runs all requested models and aggregates responses
// in the order the models were requested.  (Uses a thread-safe factory —
// the production mixture_of_agents runs sub-agents via std::async in
// parallel, so a shared Recorder would race.)
TEST(JointDelegate, MixtureOfAgentsAggregatesInOrder) {
    ToolRegistry::instance().clear();
    // Thread-safe factory: each SubAgent is self-contained, no shared state.
    auto factory = [](const std::string& model) -> std::unique_ptr<AIAgent> {
        struct SubAgent : public AIAgent {
            std::string model;
            explicit SubAgent(std::string m) : model(std::move(m)) {}
            std::string run(const std::string& goal,
                            const std::string& /*constraints*/) override {
                return "sub[" + model + "]:" + goal;
            }
        };
        return std::make_unique<SubAgent>(model);
    };
    register_delegate_tools(factory);

    auto out = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        json{{"prompt", "hello world"},
             {"models", json::array({"alpha", "beta"})}},
        ToolContext{});
    auto parsed = json::parse(out);
    ASSERT_FALSE(parsed.contains("error"));
    ASSERT_TRUE(parsed.contains("responses"));
    ASSERT_EQ(parsed["responses"].size(), 2u);

    EXPECT_EQ(parsed["responses"][0]["model"].get<std::string>(), "alpha");
    EXPECT_EQ(parsed["responses"][1]["model"].get<std::string>(), "beta");

    ToolRegistry::instance().clear();
}

// 3. `last_resolved_tool_names` is a process-global — delegate_task's
// save/restore contract means the list before and after a sub-agent run
// is identical (even if the subagent dispatches its own tools).
TEST(JointDelegate, LastResolvedToolNamesSurvivesSubagent) {
    ToolRegistry::instance().clear();
    register_delegate_tools(make_recording_factory(nullptr /* not used */));
    // Seed a "parent" resolved-tools snapshot.
    std::vector<std::string> parent_tools = {"read_file", "terminal", "memory"};
    ToolRegistry::instance().set_last_resolved_tool_names(parent_tools);

    Recorder rec;
    ToolRegistry::instance().clear();  // re-register with recording factory
    register_delegate_tools(make_recording_factory(&rec));
    ToolRegistry::instance().set_last_resolved_tool_names(parent_tools);

    auto out = ToolRegistry::instance().dispatch(
        "delegate_task",
        json{{"goal", "do a thing"}, {"model", "child"}},
        ToolContext{});
    auto parsed = json::parse(out);
    EXPECT_FALSE(parsed.contains("error"));

    // Post-dispatch: parent's resolved tool list should still be visible.
    auto after = ToolRegistry::instance().last_resolved_tool_names();
    EXPECT_EQ(after, parent_tools);

    ToolRegistry::instance().clear();
}
