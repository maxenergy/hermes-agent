#include "hermes/tools/debug_helpers.hpp"

#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace dbg = hermes::tools::debug;
using hermes::tools::ToolEntry;
using hermes::tools::ToolRegistry;

TEST(DebugHelpers, EnableToggleFlipsFlag) {
    dbg::enable_tool_call_logging(false);
    EXPECT_FALSE(dbg::tool_call_logging_enabled());
    dbg::enable_tool_call_logging(true);
    EXPECT_TRUE(dbg::tool_call_logging_enabled());
    dbg::enable_tool_call_logging(false);
}

TEST(DebugHelpers, LogToolCallIsNoopWhenDisabled) {
    dbg::enable_tool_call_logging(false);
    // Must not crash and must not throw.
    dbg::log_tool_call("x", {{"a", 1}}, "result", std::chrono::milliseconds(1));
    SUCCEED();
}

TEST(DebugHelpers, DumpRegistryStateListsRegisteredTools) {
    ToolRegistry::instance().clear();

    ToolEntry e;
    e.name = "debug_probe";
    e.toolset = "debug";
    e.schema = {{"type", "object"}};
    e.handler = [](const nlohmann::json&, const hermes::tools::ToolContext&) {
        return std::string("{\"ok\":true}");
    };
    ToolRegistry::instance().register_tool(std::move(e));

    auto dump = dbg::dump_registry_state();
    EXPECT_NE(dump.find("debug_probe"), std::string::npos);
    EXPECT_NE(dump.find("[debug]"), std::string::npos);

    ToolRegistry::instance().clear();
}

TEST(DebugHelpers, DispatchInvokesLoggerWhenEnabled) {
    ToolRegistry::instance().clear();
    int call_count = 0;
    ToolEntry e;
    e.name = "count_probe";
    e.toolset = "debug";
    e.schema = {{"type", "object"}};
    e.handler = [&call_count](const nlohmann::json&,
                              const hermes::tools::ToolContext&) {
        ++call_count;
        return std::string("{\"ok\":true}");
    };
    ToolRegistry::instance().register_tool(std::move(e));

    dbg::enable_tool_call_logging(true);
    auto out = ToolRegistry::instance().dispatch("count_probe", {}, {});
    EXPECT_EQ(call_count, 1);
    EXPECT_NE(out.find("ok"), std::string::npos);
    dbg::enable_tool_call_logging(false);

    ToolRegistry::instance().clear();
}
