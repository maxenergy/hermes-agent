// Tests for the delegate_task / mixture_of_agents tools.
//
// Covers the full policy surface ported from tools/delegate_tool.py.

#include "hermes/tools/delegate_tool.hpp"
#include "hermes/tools/registry.hpp"
#include "hermes/llm/credential_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>

using namespace hermes::tools;

namespace {

class DelegateEnvGuard {
public:
    DelegateEnvGuard() {
        const char* v = std::getenv("DELEGATION_MAX_CONCURRENT_CHILDREN");
        had_ = v != nullptr;
        if (had_) saved_ = v;
        ::unsetenv("DELEGATION_MAX_CONCURRENT_CHILDREN");
    }
    ~DelegateEnvGuard() {
        if (had_) ::setenv("DELEGATION_MAX_CONCURRENT_CHILDREN",
                           saved_.c_str(), 1);
        else      ::unsetenv("DELEGATION_MAX_CONCURRENT_CHILDREN");
    }
private:
    bool        had_ = false;
    std::string saved_;
};

struct MockAgent : public AIAgent {
    std::string model_;
    std::atomic<int>* counter_;
    MockAgent(std::string m, std::atomic<int>* c = nullptr)
        : model_(std::move(m)), counter_(c) {}
    std::string run(const std::string& goal,
                    const std::string& /*c*/) override {
        if (counter_) ++*counter_;
        return "result[" + model_ + "]:" + goal;
    }
};

AgentFactory make_mock_factory(std::atomic<int>* counter = nullptr) {
    return [counter](const std::string& model) -> std::unique_ptr<AIAgent> {
        return std::make_unique<MockAgent>(model.empty() ? "default" : model,
                                           counter);
    };
}

class DelegateToolTest : public ::testing::Test {
protected:
    DelegateEnvGuard env_guard_;
    void SetUp() override {
        unregister_delegate_tools();
        ToolRegistry::instance().clear();
        register_delegate_tools();
    }
    void TearDown() override {
        unregister_delegate_tools();
        ToolRegistry::instance().clear();
    }
};

class DelegateToolWithFactoryTest : public ::testing::Test {
protected:
    DelegateEnvGuard env_guard_;
    void SetUp() override {
        unregister_delegate_tools();
        ToolRegistry::instance().clear();
        register_delegate_tools(make_mock_factory());
    }
    void TearDown() override {
        unregister_delegate_tools();
        ToolRegistry::instance().clear();
    }
};

}  // namespace

// --------------------- registration / dispatch contract -------------------

TEST_F(DelegateToolTest, NoFactoryReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "summarize"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("requires agent factory"),
              std::string::npos);
}

TEST_F(DelegateToolTest, MoANoFactoryReturnsError) {
    auto result = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        {{"prompt", "hello"}, {"models", nlohmann::json::array({"a", "b"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(DelegateToolTest, InterfaceCompiles) {
    struct TestAgent : public AIAgent {
        std::string run(const std::string& goal,
                        const std::string& /*c*/) override {
            return "done: " + goal;
        }
    };
    TestAgent agent;
    EXPECT_EQ(agent.run("test", ""), "done: test");
}

TEST_F(DelegateToolWithFactoryTest, DelegateTaskReturnsResponse) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task",
        {{"goal", "summarize docs"}, {"constraints", "be brief"}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_FALSE(parsed.contains("error"));
    ASSERT_TRUE(parsed.contains("results"));
    ASSERT_EQ(parsed["results"].size(), 1u);
    EXPECT_EQ(parsed["results"][0]["status"].get<std::string>(), "completed");
    EXPECT_NE(parsed["results"][0]["summary"].get<std::string>().find("summarize"),
              std::string::npos);
}

TEST_F(DelegateToolWithFactoryTest, DelegateTaskMissingGoalErrors) {
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", nlohmann::json::object(), {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
}

TEST_F(DelegateToolWithFactoryTest, MoACallsMultipleModels) {
    auto result = ToolRegistry::instance().dispatch(
        "mixture_of_agents",
        {{"prompt", "hello"},
         {"models", nlohmann::json::array({"model-a", "model-b"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    EXPECT_FALSE(parsed.contains("error"));
    ASSERT_EQ(parsed["responses"].size(), 2u);
    EXPECT_EQ(parsed["model_count"].get<int>(), 2);
    EXPECT_EQ(parsed["responses"][0]["model"].get<std::string>(), "model-a");
    EXPECT_EQ(parsed["responses"][1]["model"].get<std::string>(), "model-b");
}

// --------------------- prompt assembly ------------------------------------

TEST(DelegatePrompt, GoalOnly) {
    const auto p = build_child_system_prompt("do X", "", "");
    EXPECT_NE(p.find("YOUR TASK:\ndo X"), std::string::npos);
    EXPECT_EQ(p.find("CONTEXT:"), std::string::npos);
    EXPECT_EQ(p.find("WORKSPACE PATH:"), std::string::npos);
}

TEST(DelegatePrompt, WithContextAndWorkspace) {
    const auto p = build_child_system_prompt("do X", "background", "/tmp/repo");
    EXPECT_NE(p.find("CONTEXT:\nbackground"), std::string::npos);
    EXPECT_NE(p.find("WORKSPACE PATH:\n/tmp/repo"), std::string::npos);
}

TEST(DelegatePrompt, BlankContextIgnored) {
    const auto p = build_child_system_prompt("x", "   \t\n ", "");
    EXPECT_EQ(p.find("CONTEXT:"), std::string::npos);
}

// --------------------- blocked-tool filter --------------------------------

TEST(DelegateBlockedTools, BuiltinBlocklist) {
    EXPECT_TRUE(builtin_blocked_tools().count("delegate_task"));
    EXPECT_TRUE(builtin_blocked_tools().count("clarify"));
    EXPECT_TRUE(builtin_blocked_tools().count("memory"));
    EXPECT_TRUE(builtin_blocked_tools().count("send_message"));
    EXPECT_TRUE(builtin_blocked_tools().count("execute_code"));
}

TEST(DelegateBlockedTools, StripsBuiltinToolsets) {
    auto out = strip_blocked_tools(
        {"terminal", "file", "delegation", "clarify", "memory",
         "code_execution", "web"});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "terminal");
    EXPECT_EQ(out[1], "file");
    EXPECT_EQ(out[2], "web");
}

TEST(DelegateBlockedTools, StripsExtras) {
    auto out = strip_blocked_tools({"terminal", "web", "browser"}, {"web"});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "terminal");
    EXPECT_EQ(out[1], "browser");
}

TEST(DelegateBlockedTools, DelegateTaskAlwaysBlocked) {
    auto out = strip_blocked_tools({"terminal", "delegate_task"});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "terminal");
}

// --------------------- concurrency governor -------------------------------

TEST(DelegateConcurrency, DefaultIsThree) {
    DelegateEnvGuard g;
    EXPECT_EQ(get_max_concurrent_children({}), 3);
}

TEST(DelegateConcurrency, ConfigOverridesEnv) {
    DelegateEnvGuard g;
    ::setenv("DELEGATION_MAX_CONCURRENT_CHILDREN", "7", 1);
    DelegateConfig cfg;
    cfg.max_concurrent_children = 2;
    EXPECT_EQ(get_max_concurrent_children(cfg), 2);
}

TEST(DelegateConcurrency, EnvFallback) {
    DelegateEnvGuard g;
    ::setenv("DELEGATION_MAX_CONCURRENT_CHILDREN", "5", 1);
    EXPECT_EQ(get_max_concurrent_children({}), 5);
}

TEST(DelegateConcurrency, MinimumOne) {
    DelegateEnvGuard g;
    DelegateConfig cfg;
    cfg.max_concurrent_children = -4;
    EXPECT_EQ(get_max_concurrent_children(cfg), 1);
}

// --------------------- credentials resolver -------------------------------

TEST(DelegateCreds, NoOverrideReturnsEmpty) {
    DelegateConfig cfg;
    ParentContext parent;
    parent.provider = "anthropic";
    parent.api_key  = "parent-key";
    auto creds = resolve_delegation_credentials(cfg, parent);
    EXPECT_TRUE(creds.provider.empty());
    EXPECT_TRUE(creds.api_key.empty());
}

TEST(DelegateCreds, BaseUrlPathAnthropic) {
    DelegateConfig cfg;
    cfg.base_url = "https://api.anthropic.com/v1";
    cfg.api_key  = "k";
    auto creds = resolve_delegation_credentials(cfg, {});
    EXPECT_EQ(creds.provider, "anthropic");
    EXPECT_EQ(creds.api_mode, "anthropic_messages");
    EXPECT_EQ(creds.api_key, "k");
}

TEST(DelegateCreds, BaseUrlPathCodex) {
    DelegateConfig cfg;
    cfg.base_url = "https://chatgpt.com/backend-api/codex/";
    cfg.api_key  = "tok";
    auto creds = resolve_delegation_credentials(cfg, {});
    EXPECT_EQ(creds.provider, "openai-codex");
    EXPECT_EQ(creds.api_mode, "codex_responses");
}

TEST(DelegateCreds, BaseUrlRequiresKey) {
    DelegateConfig cfg;
    cfg.base_url = "https://custom.example.com/v1";
    ::unsetenv("OPENAI_API_KEY");
    EXPECT_THROW(resolve_delegation_credentials(cfg, {}),
                 std::invalid_argument);
}

TEST(DelegateCreds, BaseUrlAcceptsEnvKey) {
    DelegateConfig cfg;
    cfg.base_url = "https://custom.example.com/v1";
    ::setenv("OPENAI_API_KEY", "env-key", 1);
    auto creds = resolve_delegation_credentials(cfg, {});
    ::unsetenv("OPENAI_API_KEY");
    EXPECT_EQ(creds.provider, "custom");
    EXPECT_EQ(creds.api_mode, "chat_completions");
    EXPECT_EQ(creds.api_key, "env-key");
}

TEST(DelegateCreds, ProviderWithoutKeyThrows) {
    DelegateConfig cfg;
    cfg.provider = "openrouter";
    EXPECT_THROW(resolve_delegation_credentials(cfg, {}),
                 std::invalid_argument);
}

// --------------------- credential pool routing ----------------------------

TEST(DelegatePool, SameProviderSharesPool) {
    hermes::llm::CredentialPool pool;
    ParentContext parent;
    parent.provider        = "openrouter";
    parent.credential_pool = &pool;
    EXPECT_EQ(resolve_child_credential_pool("openrouter", parent), &pool);
}

TEST(DelegatePool, EmptyChildProviderFallsBackToParent) {
    hermes::llm::CredentialPool pool;
    ParentContext parent;
    parent.credential_pool = &pool;
    EXPECT_EQ(resolve_child_credential_pool("", parent), &pool);
}

// --------------------- parse tasks array ----------------------------------

TEST(DelegateTasksParse, ValidBatch) {
    nlohmann::json tasks = nlohmann::json::array();
    tasks.push_back({{"goal", "a"}});
    tasks.push_back({{"goal", "b"}, {"context", "c"}});
    std::string err;
    auto out = parse_tasks_array(tasks, err);
    EXPECT_TRUE(err.empty());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].goal, "a");
    EXPECT_EQ(out[1].context, "c");
}

TEST(DelegateTasksParse, MissingGoalErrors) {
    nlohmann::json tasks = nlohmann::json::array();
    tasks.push_back({{"context", "only"}});
    std::string err;
    auto out = parse_tasks_array(tasks, err);
    EXPECT_TRUE(out.empty());
    EXPECT_NE(err.find("missing a 'goal'"), std::string::npos);
}

TEST(DelegateTasksParse, NotAnArrayErrors) {
    std::string err;
    auto out = parse_tasks_array(nlohmann::json::object(), err);
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(err.empty());
}

// --------------------- dispatch: depth / concurrency / batch --------------

TEST(DelegateDispatch, DepthCapRejectsNestedDelegation) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();
    DelegateOptions opts;
    opts.legacy_factory  = make_mock_factory();
    opts.parent_accessor = []() -> ParentContext {
        ParentContext p;
        p.depth = 2;
        return p;
    };
    register_delegate_tools(std::move(opts));

    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "x"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("depth"),
              std::string::npos);
    unregister_delegate_tools();
}

TEST(DelegateDispatch, BatchExceedingConcurrencyRejected) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();
    DelegateOptions opts;
    opts.legacy_factory                 = make_mock_factory();
    opts.config.max_concurrent_children = 2;
    register_delegate_tools(std::move(opts));

    nlohmann::json tasks = nlohmann::json::array();
    tasks.push_back({{"goal", "a"}});
    tasks.push_back({{"goal", "b"}});
    tasks.push_back({{"goal", "c"}});
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"tasks", tasks}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_NE(parsed["error"].get<std::string>().find("Too many tasks"),
              std::string::npos);
    unregister_delegate_tools();
}

TEST(DelegateDispatch, BatchRunsInParallelAndPreservesOrder) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();
    std::atomic<int> counter{0};
    DelegateOptions opts;
    opts.legacy_factory                 = make_mock_factory(&counter);
    opts.config.max_concurrent_children = 3;
    register_delegate_tools(std::move(opts));

    nlohmann::json tasks = nlohmann::json::array();
    tasks.push_back({{"goal", "alpha"}});
    tasks.push_back({{"goal", "beta"}});
    tasks.push_back({{"goal", "gamma"}});
    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"tasks", tasks}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error"));
    ASSERT_EQ(parsed["results"].size(), 3u);
    EXPECT_EQ(counter.load(), 3);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(parsed["results"][i]["task_index"].get<int>(),
                  static_cast<int>(i));
        EXPECT_EQ(parsed["results"][i]["status"].get<std::string>(),
                  "completed");
    }
    unregister_delegate_tools();
}

// --------------------- rich factory + progress forwarding -----------------

namespace {

struct RichAgent : public AIAgent {
    ChildCredentials last_creds;
    std::vector<std::string> last_toolsets;
    std::string last_system;
    std::string run(const std::string&, const std::string&) override { return ""; }
    ChildResult run_with_context(const std::string& goal,
                                 const std::string& sp,
                                 const std::vector<std::string>& toolsets,
                                 const ChildCredentials& creds,
                                 ProgressCallback cb) override {
        last_creds    = creds;
        last_toolsets = toolsets;
        last_system   = sp;
        if (cb) {
            ProgressEvent e;
            e.event_type = "tool.started";
            e.tool_name  = "read_file";
            e.preview    = "foo.txt";
            cb(e);
        }
        ChildResult r;
        r.summary     = "rich-ran: " + goal;
        r.status      = "completed";
        r.exit_reason = "completed";
        r.model       = creds.model;
        return r;
    }
};

}  // namespace

TEST(DelegateRichFactory, ToolsetsFilteredThroughBlocklist) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();

    // Child is short-lived — capture its observed state into a shared
    // vector so we can inspect it after the agent has been destroyed.
    auto captured_toolsets = std::make_shared<std::vector<std::string>>();
    DelegateOptions opts;
    opts.rich_factory = [captured_toolsets](const ChildCredentials&,
                                            const ParentContext&,
                                            int) -> std::unique_ptr<AIAgent> {
        struct CapturingAgent : public AIAgent {
            std::shared_ptr<std::vector<std::string>> out;
            std::string run(const std::string&, const std::string&) override { return ""; }
            ChildResult run_with_context(const std::string& goal,
                                         const std::string&,
                                         const std::vector<std::string>& toolsets,
                                         const ChildCredentials& creds,
                                         ProgressCallback) override {
                *out = toolsets;
                ChildResult r;
                r.status = "completed";
                r.summary = "ok:" + goal;
                r.exit_reason = "completed";
                r.model = creds.model;
                return r;
            }
        };
        auto a = std::make_unique<CapturingAgent>();
        a->out = captured_toolsets;
        return a;
    };
    register_delegate_tools(std::move(opts));

    auto result = ToolRegistry::instance().dispatch(
        "delegate_task",
        {{"goal", "go"},
         {"toolsets", nlohmann::json::array({"terminal", "memory", "clarify"})}},
        {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error"));
    ASSERT_EQ(captured_toolsets->size(), 1u);
    EXPECT_EQ((*captured_toolsets)[0], "terminal");

    unregister_delegate_tools();
}

TEST(DelegateProgress, ParentCallbackReceivesChildToolEvent) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();

    std::atomic<int> received{0};
    std::string seen_tool;
    DelegateOptions opts;
    opts.rich_factory = [](const ChildCredentials&, const ParentContext&, int)
        -> std::unique_ptr<AIAgent> {
        return std::make_unique<RichAgent>();
    };
    opts.parent_accessor = [&received, &seen_tool]() -> ParentContext {
        ParentContext p;
        p.progress_callback = [&received, &seen_tool](const ProgressEvent& e) {
            ++received;
            seen_tool = e.tool_name;
        };
        return p;
    };
    register_delegate_tools(std::move(opts));

    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "go"}}, {});
    (void)result;
    EXPECT_GE(received.load(), 1);
    EXPECT_EQ(seen_tool, "read_file");

    unregister_delegate_tools();
}

TEST(DelegateProgress, CompletedEventSuppressed) {
    DelegateEnvGuard g;
    unregister_delegate_tools();
    ToolRegistry::instance().clear();

    std::atomic<int> completed_seen{0};
    struct EmitCompletedAgent : public AIAgent {
        std::string run(const std::string&, const std::string&) override { return ""; }
        ChildResult run_with_context(const std::string& goal,
                                     const std::string& /*sp*/,
                                     const std::vector<std::string>&,
                                     const ChildCredentials& creds,
                                     ProgressCallback cb) override {
            if (cb) {
                ProgressEvent e; e.event_type = "tool.completed"; cb(e);
            }
            ChildResult r; r.status = "completed"; r.summary = goal;
            r.model = creds.model;
            return r;
        }
    };
    DelegateOptions opts;
    opts.rich_factory = [](const ChildCredentials&, const ParentContext&, int)
        -> std::unique_ptr<AIAgent> {
        return std::make_unique<EmitCompletedAgent>();
    };
    opts.parent_accessor = [&completed_seen]() -> ParentContext {
        ParentContext p;
        p.progress_callback = [&completed_seen](const ProgressEvent& e) {
            if (e.event_type == "tool.completed") ++completed_seen;
        };
        return p;
    };
    register_delegate_tools(std::move(opts));

    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "g"}}, {});
    (void)result;
    EXPECT_EQ(completed_seen.load(), 0);

    unregister_delegate_tools();
}

// --------------------- process-global tool-name save/restore --------------

TEST_F(DelegateToolWithFactoryTest, LastResolvedToolNamesSurvivesDispatch) {
    std::vector<std::string> parent_names = {"read_file", "terminal"};
    ToolRegistry::instance().set_last_resolved_tool_names(parent_names);

    auto result = ToolRegistry::instance().dispatch(
        "delegate_task", {{"goal", "x"}}, {});
    auto parsed = nlohmann::json::parse(result);
    ASSERT_FALSE(parsed.contains("error"));

    auto after = ToolRegistry::instance().last_resolved_tool_names();
    EXPECT_EQ(after, parent_names);
}

// --------------------- config loader --------------------------------------

TEST(DelegateConfigJson, FromJsonHonoursAllKeys) {
    nlohmann::json j = {
        {"max_concurrent_children", 5},
        {"max_iterations", 77},
        {"max_depth", 3},
        {"model",    "gpt-4o"},
        {"provider", "openrouter"},
        {"blocked_tools", nlohmann::json::array({"web"})},
    };
    auto cfg = DelegateConfig::from_json(j);
    ASSERT_TRUE(cfg.max_concurrent_children.has_value());
    EXPECT_EQ(*cfg.max_concurrent_children, 5);
    EXPECT_EQ(*cfg.max_iterations, 77);
    EXPECT_EQ(*cfg.max_depth, 3);
    EXPECT_EQ(cfg.model, "gpt-4o");
    EXPECT_EQ(cfg.provider, "openrouter");
    ASSERT_EQ(cfg.extra_blocked_tools.size(), 1u);
    EXPECT_EQ(cfg.extra_blocked_tools[0], "web");
}

TEST(DelegateConfigJson, EmptyJsonYieldsDefaults) {
    auto cfg = DelegateConfig::from_json(nlohmann::json::object());
    EXPECT_FALSE(cfg.max_concurrent_children.has_value());
    EXPECT_TRUE(cfg.model.empty());
}
