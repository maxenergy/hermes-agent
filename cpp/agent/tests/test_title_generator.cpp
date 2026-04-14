#include "hermes/agent/title_generator.hpp"

#include "hermes/llm/llm_client.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using hermes::agent::generate_title;
using hermes::llm::CompletionRequest;
using hermes::llm::CompletionResponse;
using hermes::llm::LlmClient;
using hermes::llm::Message;
using hermes::llm::Role;

namespace {

class StubClient : public LlmClient {
public:
    explicit StubClient(std::string body) : body_(std::move(body)) {}
    CompletionResponse complete(const CompletionRequest&) override {
        CompletionResponse out;
        out.assistant_message.role = Role::Assistant;
        out.assistant_message.content_text = body_;
        out.finish_reason = "stop";
        return out;
    }
    std::string provider_name() const override { return "stub"; }

private:
    std::string body_;
};

class ThrowingClient : public LlmClient {
public:
    CompletionResponse complete(const CompletionRequest&) override {
        throw std::runtime_error("nope");
    }
    std::string provider_name() const override { return "throw"; }
};

}  // namespace

TEST(TitleGenerator, ReturnsTrimmedTitle) {
    StubClient s("My Title");
    auto t = generate_title(&s, "gpt-aux", "What is the meaning of life?");
    EXPECT_EQ(t, "My Title");
}

TEST(TitleGenerator, StripsQuotesAndPrefix) {
    StubClient s("\"Title: Project Setup\"");
    auto t = generate_title(&s, "gpt-aux", "Help me set up the project.");
    EXPECT_EQ(t, "Project Setup");
}

TEST(TitleGenerator, TruncatesAt80Chars) {
    StubClient s(std::string(120, 'a'));
    auto t = generate_title(&s, "gpt-aux", "noise");
    EXPECT_LE(t.size(), 80u);
}

TEST(TitleGenerator, PostprocessHandlesTitlePrefix) {
    EXPECT_EQ(hermes::agent::postprocess_title(" Title: Hello "), "Hello");
    EXPECT_EQ(hermes::agent::postprocess_title("\"Quoted\""), "Quoted");
    EXPECT_EQ(hermes::agent::postprocess_title("'  mixed '"), "mixed");
}

TEST(TitleGenerator, ShouldAutoTitleByHistoryLength) {
    std::vector<hermes::llm::Message> hist;
    EXPECT_TRUE(hermes::agent::should_auto_title(hist));
    hermes::llm::Message u;
    u.role = hermes::llm::Role::User;
    hist.push_back(u);
    EXPECT_TRUE(hermes::agent::should_auto_title(hist));
    hist.push_back(u);
    EXPECT_TRUE(hermes::agent::should_auto_title(hist));
    hist.push_back(u);  // 3 user msgs — too late.
    EXPECT_FALSE(hermes::agent::should_auto_title(hist));
}

TEST(TitleGenerator, AutoTitleSkipsWhenAlreadySet) {
    hermes::agent::SessionTitleStore store;
    bool called_set = false;
    store.get_title = [](const std::string&) { return "Existing"; };
    store.set_title = [&](const std::string&, const std::string&) {
        called_set = true;
        return true;
    };
    StubClient s("Generated");
    hermes::agent::auto_title_session(store, &s, "aux", "sess-1", "u", "a");
    EXPECT_FALSE(called_set);
}

TEST(TitleGenerator, AutoTitleStoresGenerated) {
    hermes::agent::SessionTitleStore store;
    std::string stored;
    store.get_title = [](const std::string&) { return std::string(); };
    store.set_title = [&](const std::string&, const std::string& t) {
        stored = t;
        return true;
    };
    StubClient s("Project Setup");
    hermes::agent::auto_title_session(store, &s, "aux", "sess-1", "help", "sure");
    EXPECT_EQ(stored, "Project Setup");
}

TEST(TitleGenerator, EmptyOnException) {
    ThrowingClient c;
    EXPECT_TRUE(generate_title(&c, "gpt-aux", "x").empty());
}

TEST(TitleGenerator, EmptyWhenClientNull) {
    EXPECT_TRUE(generate_title(nullptr, "gpt-aux", "x").empty());
}
