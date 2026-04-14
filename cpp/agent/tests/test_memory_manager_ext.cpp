// Extra tests covering the expanded MemoryManager API (tool dispatch,
// lifecycle hooks, prefetch_all_string, fencing helpers).
#include "hermes/agent/memory_manager.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

class RecordingProvider : public hermes::agent::MemoryProvider {
public:
    explicit RecordingProvider(std::string n, bool external = true)
        : name_(std::move(n)), external_(external) {}

    std::string name() const override { return name_; }
    bool is_external() const override { return external_; }
    std::string build_system_prompt_section() override { return section_; }

    std::string prefetch_string(const std::string&,
                                const std::string&) override {
        return fetch_text_;
    }
    std::vector<nlohmann::json> get_tool_schemas() const override {
        return schemas_;
    }
    std::string handle_tool_call(const std::string& name,
                                 const nlohmann::json&,
                                 const hermes::agent::MemoryProviderContext&) override {
        last_tool_ = name;
        return nlohmann::json{{"handled", name}}.dump();
    }
    void initialize(const hermes::agent::MemoryProviderContext&) override {
        ++init_count_;
    }
    void shutdown() override { ++shutdown_count_; }
    void on_turn_start(int, const std::string&,
                       const hermes::agent::MemoryProviderContext&) override {
        ++turn_count_;
    }
    std::string on_pre_compress(
        const std::vector<hermes::llm::Message>&) override {
        return pre_compress_text_;
    }
    void on_memory_write(const std::string&, const std::string&,
                         const std::string&) override {
        ++write_count_;
    }

    std::string name_;
    bool external_;
    std::string section_;
    std::string fetch_text_;
    std::string pre_compress_text_;
    std::vector<nlohmann::json> schemas_;
    std::string last_tool_;
    int init_count_ = 0;
    int shutdown_count_ = 0;
    int turn_count_ = 0;
    int write_count_ = 0;
};

}  // namespace

TEST(MemoryManagerExt, SanitizeStripsFence) {
    EXPECT_EQ(hermes::agent::sanitize_memory_context(
                  "<memory-context>abc</memory-context>"),
              "abc");
    EXPECT_EQ(hermes::agent::sanitize_memory_context(
                  "<MEMORY-CONTEXT>x</memory-context>"),
              "x");
}

TEST(MemoryManagerExt, BuildBlockEmptyForBlank) {
    EXPECT_EQ(hermes::agent::build_memory_context_block(""), "");
    EXPECT_EQ(hermes::agent::build_memory_context_block("   \n  "), "");
}

TEST(MemoryManagerExt, BuildBlockWrapsContent) {
    std::string out = hermes::agent::build_memory_context_block("hello");
    EXPECT_NE(out.find("<memory-context>"), std::string::npos);
    EXPECT_NE(out.find("</memory-context>"), std::string::npos);
    EXPECT_NE(out.find("System note"), std::string::npos);
    EXPECT_NE(out.find("hello"), std::string::npos);
}

TEST(MemoryManagerExt, PrefetchAllStringConcatsAndFences) {
    hermes::agent::MemoryManager m;
    auto a = std::make_unique<RecordingProvider>("p_a", true);
    a->fetch_text_ = "fact A";
    m.add_provider(std::move(a));
    auto s = m.prefetch_all_string("q");
    EXPECT_NE(s.find("fact A"), std::string::npos);
    EXPECT_NE(s.find("<memory-context>"), std::string::npos);
}

TEST(MemoryManagerExt, ToolIndexDispatchesToOwner) {
    hermes::agent::MemoryManager m;
    auto p = std::make_unique<RecordingProvider>("with_tools");
    p->schemas_ = {
        {{"name", "magic_tool"}, {"description", "x"}},
    };
    auto* raw = p.get();
    m.add_provider(std::move(p));
    m.rebuild_tool_index();

    auto all = m.get_all_tool_schemas();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0]["name"], "magic_tool");

    hermes::agent::MemoryProviderContext ctx;
    std::string result = m.handle_tool_call(
        "magic_tool", nlohmann::json::object(), ctx);
    EXPECT_EQ(raw->last_tool_, "magic_tool");
    auto parsed = nlohmann::json::parse(result);
    EXPECT_EQ(parsed["handled"], "magic_tool");
}

TEST(MemoryManagerExt, ToolDispatchUnknownReturnsError) {
    hermes::agent::MemoryManager m;
    hermes::agent::MemoryProviderContext ctx;
    std::string r = m.handle_tool_call("nope", nlohmann::json::object(), ctx);
    auto j = nlohmann::json::parse(r);
    EXPECT_TRUE(j.contains("error"));
}

TEST(MemoryManagerExt, LifecycleHooksFanOut) {
    hermes::agent::MemoryManager m;
    auto p = std::make_unique<RecordingProvider>("lifecycle");
    auto* raw = p.get();
    m.add_provider(std::move(p));
    hermes::agent::MemoryProviderContext ctx;
    m.initialize_all(ctx);
    m.on_turn_start_all(3, "u", ctx);
    m.on_memory_write_all("add", "memory", "body");
    m.shutdown_all();
    EXPECT_EQ(raw->init_count_, 1);
    EXPECT_EQ(raw->turn_count_, 1);
    EXPECT_EQ(raw->write_count_, 1);
    EXPECT_EQ(raw->shutdown_count_, 1);
}

TEST(MemoryManagerExt, OnPreCompressAggregates) {
    hermes::agent::MemoryManager m;
    auto p = std::make_unique<RecordingProvider>("pc");
    p->pre_compress_text_ = "insight";
    m.add_provider(std::move(p));
    std::vector<hermes::llm::Message> msgs;
    EXPECT_EQ(m.on_pre_compress_all(msgs), "insight");
}

TEST(MemoryManagerExt, ProviderNamesListsOrder) {
    hermes::agent::MemoryManager m;
    m.add_provider(std::make_unique<RecordingProvider>("first", true));
    auto names = m.provider_names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "first");
    EXPECT_TRUE(m.has_external_provider());
}
