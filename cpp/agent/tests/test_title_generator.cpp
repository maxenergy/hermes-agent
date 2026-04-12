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

TEST(TitleGenerator, TruncatesAt60Chars) {
    StubClient s(std::string(120, 'a'));
    auto t = generate_title(&s, "gpt-aux", "noise");
    EXPECT_LE(t.size(), 60u);
}

TEST(TitleGenerator, EmptyOnException) {
    ThrowingClient c;
    EXPECT_TRUE(generate_title(&c, "gpt-aux", "x").empty());
}

TEST(TitleGenerator, EmptyWhenClientNull) {
    EXPECT_TRUE(generate_title(nullptr, "gpt-aux", "x").empty());
}
