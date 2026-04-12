#include "hermes/tools/registry.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>

using namespace hermes::tools;

namespace {

class RegistryTest : public ::testing::Test {
protected:
    void SetUp() override { ToolRegistry::instance().clear(); }
    void TearDown() override { ToolRegistry::instance().clear(); }
};

ToolEntry make_entry(std::string name, std::string toolset = "test") {
    ToolEntry e;
    e.name = std::move(name);
    e.toolset = std::move(toolset);
    e.description = "test tool";
    e.schema = nlohmann::json::object();
    e.schema["parameters"] = nlohmann::json::object();
    e.handler = [](const nlohmann::json&, const ToolContext&) {
        return std::string(R"({"ok":true})");
    };
    return e;
}

TEST_F(RegistryTest, RegisterAndDispatchHappyPath) {
    auto& reg = ToolRegistry::instance();
    reg.register_tool(make_entry("hello"));

    const std::string out = reg.dispatch("hello", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(out);
    EXPECT_TRUE(parsed.contains("ok"));
    EXPECT_TRUE(parsed["ok"].get<bool>());
}

TEST_F(RegistryTest, UnknownToolReturnsErrorJson) {
    auto& reg = ToolRegistry::instance();
    const std::string out = reg.dispatch("nope", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(out);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("nope"), std::string::npos);
}

TEST_F(RegistryTest, CheckFnFailureProducesError) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("gated");
    e.check_fn = [] { return false; };
    reg.register_tool(std::move(e));

    auto parsed = nlohmann::json::parse(
        reg.dispatch("gated", nlohmann::json::object(), {}));
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(RegistryTest, MissingRequiredEnvProducesError) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("env_dep");
    e.requires_env = {"HERMES_TEST_DEFINITELY_NOT_SET_VAR_XYZ"};
    reg.register_tool(std::move(e));

    auto parsed = nlohmann::json::parse(
        reg.dispatch("env_dep", nlohmann::json::object(), {}));
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("HERMES_TEST"),
              std::string::npos);
}

TEST_F(RegistryTest, HandlerThrowIsCaught) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("boom");
    e.handler = [](const nlohmann::json&, const ToolContext&) -> std::string {
        throw std::runtime_error("kaboom");
    };
    reg.register_tool(std::move(e));

    auto parsed = nlohmann::json::parse(
        reg.dispatch("boom", nlohmann::json::object(), {}));
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("kaboom"),
              std::string::npos);
}

TEST_F(RegistryTest, OversizedResultIsTruncated) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("big");
    e.max_result_size_chars = 64;
    e.handler = [](const nlohmann::json&, const ToolContext&) {
        // Build a JSON object string longer than 64 chars.
        nlohmann::json j;
        j["payload"] = std::string(1024, 'x');
        return j.dump();
    };
    reg.register_tool(std::move(e));

    const std::string out = reg.dispatch("big", nlohmann::json::object(), {});
    EXPECT_LE(out.size(), 64u);
    EXPECT_NE(out.find("truncated"), std::string::npos);
}

TEST_F(RegistryTest, EmptyResultBecomesOkTrue) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("blank");
    e.handler = [](const nlohmann::json&, const ToolContext&) {
        return std::string();
    };
    reg.register_tool(std::move(e));

    auto parsed = nlohmann::json::parse(
        reg.dispatch("blank", nlohmann::json::object(), {}));
    EXPECT_TRUE(parsed.contains("ok"));
    EXPECT_TRUE(parsed["ok"].get<bool>());
}

TEST_F(RegistryTest, NonJsonResultIsWrappedInOutput) {
    auto& reg = ToolRegistry::instance();
    auto e = make_entry("plain");
    e.handler = [](const nlohmann::json&, const ToolContext&) {
        return std::string("hello world");
    };
    reg.register_tool(std::move(e));

    auto parsed = nlohmann::json::parse(
        reg.dispatch("plain", nlohmann::json::object(), {}));
    ASSERT_TRUE(parsed.contains("output"));
    EXPECT_EQ(parsed["output"].get<std::string>(), "hello world");
}

TEST_F(RegistryTest, GetDefinitionsFiltersByToolset) {
    auto& reg = ToolRegistry::instance();
    reg.register_tool(make_entry("a", "alpha"));
    reg.register_tool(make_entry("b", "beta"));
    reg.register_tool(make_entry("c", "alpha"));

    const auto defs = reg.get_definitions({"alpha"}, {});
    ASSERT_EQ(defs.size(), 2u);
    EXPECT_EQ(defs[0].name, "a");
    EXPECT_EQ(defs[1].name, "c");

    const auto excluded = reg.get_definitions({}, {"beta"});
    ASSERT_EQ(excluded.size(), 2u);
}

TEST_F(RegistryTest, GetDefinitionsFiltersOutFailingCheckFn) {
    auto& reg = ToolRegistry::instance();
    auto good = make_entry("good");
    auto bad = make_entry("bad");
    bad.check_fn = [] { return false; };
    reg.register_tool(std::move(good));
    reg.register_tool(std::move(bad));

    const auto defs = reg.get_definitions({}, {});
    ASSERT_EQ(defs.size(), 1u);
    EXPECT_EQ(defs[0].name, "good");
}

TEST_F(RegistryTest, LastResolvedSaveAndRestore) {
    auto& reg = ToolRegistry::instance();
    reg.set_last_resolved_tool_names({"x", "y", "z"});
    auto saved = reg.last_resolved_tool_names();
    EXPECT_EQ(saved.size(), 3u);
    EXPECT_EQ(saved[0], "x");

    reg.set_last_resolved_tool_names({"q"});
    EXPECT_EQ(reg.last_resolved_tool_names().size(), 1u);

    reg.set_last_resolved_tool_names(std::move(saved));
    EXPECT_EQ(reg.last_resolved_tool_names().size(), 3u);
}

TEST_F(RegistryTest, ToolResultHelperWrapsScalarValues) {
    const std::string out = tool_result(nlohmann::json("hello"));
    auto parsed = nlohmann::json::parse(out);
    ASSERT_TRUE(parsed.contains("output"));
    EXPECT_EQ(parsed["output"].get<std::string>(), "hello");
}

TEST_F(RegistryTest, ToolResultHelperKeepsObjects) {
    nlohmann::json obj = {{"a", 1}, {"b", 2}};
    auto parsed = nlohmann::json::parse(tool_result(obj));
    EXPECT_EQ(parsed["a"].get<int>(), 1);
    EXPECT_EQ(parsed["b"].get<int>(), 2);
}

TEST_F(RegistryTest, ToolErrorHelperEmitsValidJson) {
    const std::string out = tool_error("nope", {{"code", 404}});
    auto parsed = nlohmann::json::parse(out);
    EXPECT_EQ(parsed["error"].get<std::string>(), "nope");
    EXPECT_EQ(parsed["code"].get<int>(), 404);
}

TEST_F(RegistryTest, RegisterTwiceOverwrites) {
    auto& reg = ToolRegistry::instance();
    reg.register_tool(make_entry("dup", "a"));
    reg.register_tool(make_entry("dup", "b"));
    EXPECT_EQ(reg.size(), 1u);
    EXPECT_EQ(reg.get_toolset_for_tool("dup").value_or(""), "b");
}

TEST_F(RegistryTest, ListToolsReturnsSortedNames) {
    auto& reg = ToolRegistry::instance();
    reg.register_tool(make_entry("zeta"));
    reg.register_tool(make_entry("alpha"));
    reg.register_tool(make_entry("mu"));
    auto out = reg.list_tools();
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "alpha");
    EXPECT_EQ(out[1], "mu");
    EXPECT_EQ(out[2], "zeta");
}

TEST_F(RegistryTest, ToolsetCheckBlocksDefinitions) {
    auto& reg = ToolRegistry::instance();
    reg.register_tool(make_entry("t1", "blocked"));
    reg.register_toolset_check("blocked", [] { return false; });
    EXPECT_FALSE(reg.is_toolset_available("blocked"));
    EXPECT_TRUE(reg.get_definitions({}, {}).empty());
}

}  // namespace
