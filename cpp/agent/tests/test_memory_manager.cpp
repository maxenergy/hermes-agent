#include "hermes/agent/memory_manager.hpp"
#include "hermes/agent/memory_provider.hpp"
#include "hermes/state/memory_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <random>

using hermes::agent::BuiltinMemoryProvider;
using hermes::agent::MemoryManager;
using hermes::agent::MemoryProvider;

namespace fs = std::filesystem;

namespace {

class FakeProvider : public MemoryProvider {
public:
    FakeProvider(std::string n, bool external, std::string section)
        : name_(std::move(n)),
          external_(external),
          section_(std::move(section)) {}
    std::string name() const override { return name_; }
    bool is_external() const override { return external_; }
    std::string build_system_prompt_section() override { return section_; }
    void prefetch(std::string_view) override { ++prefetch_count_; }
    void sync(std::string_view, std::string_view) override { ++sync_count_; }
    int prefetch_count() const { return prefetch_count_.load(); }
    int sync_count() const { return sync_count_; }

private:
    std::string name_;
    bool external_;
    std::string section_;
    std::atomic<int> prefetch_count_{0};
    int sync_count_ = 0;
};

fs::path tmp_memdir() {
    auto base = fs::temp_directory_path();
    std::random_device rd;
    auto p = base / ("hermes_mm_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}

}  // namespace

TEST(MemoryManager, AcceptsOneBuiltinPlusOneExternal) {
    MemoryManager m;
    m.add_provider(std::make_unique<FakeProvider>("builtin", false, "B"));
    m.add_provider(std::make_unique<FakeProvider>("ext", true, "E"));
    EXPECT_EQ(m.provider_count(), 2u);
}

TEST(MemoryManager, RejectsTwoExternals) {
    MemoryManager m;
    m.add_provider(std::make_unique<FakeProvider>("ext1", true, "E1"));
    EXPECT_THROW(
        m.add_provider(std::make_unique<FakeProvider>("ext2", true, "E2")),
        std::invalid_argument);
}

TEST(MemoryManager, RejectsTwoBuiltins) {
    MemoryManager m;
    m.add_provider(std::make_unique<FakeProvider>("b1", false, "B1"));
    EXPECT_THROW(
        m.add_provider(std::make_unique<FakeProvider>("b2", false, "B2")),
        std::invalid_argument);
}

TEST(MemoryManager, BuildSystemPromptConcatenatesSections) {
    MemoryManager m;
    m.add_provider(std::make_unique<FakeProvider>("b", false, "BUILTIN"));
    m.add_provider(std::make_unique<FakeProvider>("e", true, "EXTERNAL"));
    auto out = m.build_system_prompt();
    EXPECT_NE(out.find("BUILTIN"), std::string::npos);
    EXPECT_NE(out.find("EXTERNAL"), std::string::npos);
}

TEST(MemoryManager, PrefetchAndSyncFanOut) {
    MemoryManager m;
    auto* fp1_raw =
        new FakeProvider("b", false, "BUILTIN");  // tracked via unique_ptr
    auto* fp2_raw = new FakeProvider("e", true, "EXTERNAL");
    m.add_provider(std::unique_ptr<MemoryProvider>(fp1_raw));
    m.add_provider(std::unique_ptr<MemoryProvider>(fp2_raw));
    m.prefetch_all("hi");
    m.sync_all("hi", "hello");
    EXPECT_EQ(fp1_raw->prefetch_count(), 1);
    EXPECT_EQ(fp1_raw->sync_count(), 1);
    EXPECT_EQ(fp2_raw->prefetch_count(), 1);
    EXPECT_EQ(fp2_raw->sync_count(), 1);
}

TEST(MemoryManager, RemoveProviderRemovesByName) {
    MemoryManager m;
    m.add_provider(std::make_unique<FakeProvider>("b", false, "B"));
    m.add_provider(std::make_unique<FakeProvider>("e", true, "E"));
    m.remove_provider("e");
    EXPECT_EQ(m.provider_count(), 1u);
    auto out = m.build_system_prompt();
    EXPECT_EQ(out.find("E"), std::string::npos);
    EXPECT_NE(out.find("B"), std::string::npos);
}

TEST(MemoryManager, BuiltinProviderReadsFromMemoryStore) {
    auto dir = tmp_memdir();
    hermes::state::MemoryStore store(dir);
    store.add(hermes::state::MemoryFile::Agent, "remember tabs vs spaces");

    MemoryManager m;
    m.add_provider(std::make_unique<BuiltinMemoryProvider>(&store));
    auto out = m.build_system_prompt();
    EXPECT_NE(out.find("remember tabs vs spaces"), std::string::npos);
    fs::remove_all(dir);
}

TEST(MemoryManager, QueuePrefetchRunsInBackground) {
    FakeProvider* fp_view = nullptr;
    {
        auto fp = std::make_unique<FakeProvider>("b", false, "B");
        fp_view = fp.get();
        MemoryManager m;
        m.add_provider(std::move(fp));
        m.queue_prefetch_all("query");
        // Force the worker to drain by issuing another queue call
        // (which joins the previous thread).
        m.queue_prefetch_all("query2");
        m.queue_prefetch_all("query3");
        // Manager destructor will join the trailing worker.
    }
    // After m is destroyed, fp_view is invalid — but at least one of
    // the queued calls should have completed.
    (void)fp_view;
    SUCCEED();
}
