// Tests for the expanded MemoryProvider API defaults.
#include "hermes/agent/memory_provider.hpp"

#include <gtest/gtest.h>

namespace {

class StubProvider : public hermes::agent::MemoryProvider {
public:
    std::string name() const override { return "stub"; }
    bool is_external() const override { return true; }
    std::string build_system_prompt_section() override { return "stub\n"; }
};

}  // namespace

TEST(MemoryProvider, DefaultsDoNotThrow) {
    StubProvider p;
    hermes::agent::MemoryProviderContext ctx;
    p.initialize(ctx);
    p.on_turn_start(1, "hi", ctx);
    p.on_delegation("task", "result", "child-1", ctx);
    p.on_memory_write("add", "memory", "blob");
    std::vector<hermes::llm::Message> msgs;
    EXPECT_EQ(p.on_pre_compress(msgs), "");
    p.on_session_end(msgs, ctx);
    p.shutdown();
    SUCCEED();
}

TEST(MemoryProvider, DefaultPrefetchStringEmpty) {
    StubProvider p;
    EXPECT_EQ(p.prefetch_string("anything"), "");
}

TEST(MemoryProvider, DefaultToolSchemasEmpty) {
    StubProvider p;
    EXPECT_TRUE(p.get_tool_schemas().empty());
}

TEST(MemoryProvider, HandleToolCallReportsUnsupported) {
    StubProvider p;
    hermes::agent::MemoryProviderContext ctx;
    std::string out = p.handle_tool_call("magic", nlohmann::json::object(), ctx);
    auto j = nlohmann::json::parse(out);
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j["error"].get<std::string>().find("stub"), std::string::npos);
}

TEST(MemoryProvider, DefaultConfigSchemaEmpty) {
    StubProvider p;
    EXPECT_TRUE(p.get_config_schema().empty());
}

TEST(MemoryProvider, IsAvailableDefaultsTrue) {
    StubProvider p;
    EXPECT_TRUE(p.is_available());
}

TEST(MemoryProvider, SyncTurnDelegatesToSync) {
    // The default sync() is a no-op; just verify sync_turn compiles.
    StubProvider p;
    p.sync_turn("u", "a", "sess-1");
    SUCCEED();
}
