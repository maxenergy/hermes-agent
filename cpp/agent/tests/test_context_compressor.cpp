#include "hermes/agent/context_compressor.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"
#include "hermes/llm/openai_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <vector>

using hermes::agent::CompressionOptions;
using hermes::agent::ContextCompressor;
using hermes::llm::CompletionRequest;
using hermes::llm::CompletionResponse;
using hermes::llm::FakeHttpTransport;
using hermes::llm::HttpTransport;
using hermes::llm::LlmClient;
using hermes::llm::Message;
using hermes::llm::OpenAIClient;
using hermes::llm::Role;
using json = nlohmann::json;

namespace {

// Minimal scripted client that returns pre-baked responses without
// going through HTTP at all — keeps the compressor tests independent
// of provider wire formats.
class ScriptedClient : public LlmClient {
public:
    void enqueue(std::string content) {
        Message m;
        m.role = Role::Assistant;
        m.content_text = std::move(content);
        scripts_.push_back(std::move(m));
    }
    int call_count() const { return call_count_; }
    CompletionResponse complete(const CompletionRequest&) override {
        ++call_count_;
        if (scripts_.empty()) throw std::runtime_error("no scripted response");
        CompletionResponse out;
        out.assistant_message = scripts_.front();
        scripts_.erase(scripts_.begin());
        out.finish_reason = "stop";
        return out;
    }
    std::string provider_name() const override { return "scripted"; }

private:
    std::vector<Message> scripts_;
    int call_count_ = 0;
};

Message make_msg(Role r, std::string text) {
    Message m;
    m.role = r;
    m.content_text = std::move(text);
    return m;
}

std::vector<Message> build_long_history(int turns) {
    std::vector<Message> msgs;
    msgs.push_back(make_msg(Role::System, "system prompt"));
    for (int i = 0; i < turns; ++i) {
        msgs.push_back(make_msg(Role::User, "user turn " + std::to_string(i) +
                                                " " + std::string(200, 'x')));
        msgs.push_back(
            make_msg(Role::Assistant,
                     "assistant turn " + std::to_string(i) + " " +
                         std::string(200, 'y')));
    }
    return msgs;
}

}  // namespace

TEST(ContextCompressor, BelowThresholdReturnsUnchanged) {
    ScriptedClient sc;
    sc.enqueue("should not be called");
    ContextCompressor c(&sc, "gpt-aux");

    auto msgs = build_long_history(5);
    auto out = c.compress(msgs, /*current=*/100, /*max=*/100000);
    EXPECT_EQ(out.size(), msgs.size());
    EXPECT_EQ(sc.call_count(), 0);
}

TEST(ContextCompressor, NoMiddleReturnsUnchanged) {
    ScriptedClient sc;
    sc.enqueue("unused");
    CompressionOptions opts;
    opts.protected_tail_turns = 8;  // bigger than the history → all protected
    ContextCompressor c(&sc, "gpt-aux", opts);

    auto msgs = build_long_history(2);  // sys + 2 user + 2 assistant
    auto out = c.compress(msgs, /*current=*/9000, /*max=*/10000);
    EXPECT_EQ(out.size(), msgs.size());
    EXPECT_EQ(sc.call_count(), 0);
}

TEST(ContextCompressor, AboveThresholdSummarisesMiddle) {
    ScriptedClient sc;
    sc.enqueue("**Goal:** ship\n**Progress:** half\n");
    ContextCompressor c(&sc, "gpt-aux");

    auto msgs = build_long_history(10);
    auto out = c.compress(msgs, /*current=*/9000, /*max=*/10000);
    // Original head + 1 summary system message + protected tail.
    EXPECT_LT(out.size(), msgs.size());
    EXPECT_EQ(sc.call_count(), 1);

    // System prompt at index 0 must still be present.
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().role, Role::System);
    EXPECT_EQ(out.front().content_text, "system prompt");

    // Summary marker visible somewhere. The new pipeline mirrors the
    // Python [CONTEXT COMPACTION] prefix (see compressor_depth::
    // kSummaryPrefix) rather than the legacy "Compressed history
    // summary" header.
    bool found_summary = false;
    for (const auto& m : out) {
        if (m.content_text.find("[CONTEXT COMPACTION]") !=
            std::string::npos) {
            found_summary = true;
            break;
        }
    }
    EXPECT_TRUE(found_summary);
}

TEST(ContextCompressor, ProtectedTailIsPreserved) {
    ScriptedClient sc;
    sc.enqueue("summary body");
    CompressionOptions opts;
    // Python-aligned behaviour: the token budget is the primary
    // criterion for the tail cut. Python _find_tail_cut_by_tokens
    // walks backward accumulating tokens until soft_ceiling
    // (= budget * 1.5) is exceeded AND at least min_tail=3 messages
    // have been kept. Pick a budget that lets ~8-11 tail messages
    // survive so our 4 sentinels (interleaved with 4 assistant msgs)
    // fall inside the protected tail.
    opts.protected_tail_tokens = 500;  // soft ceiling ~= 750 tokens
    ContextCompressor c(&sc, "gpt-aux", opts);

    auto msgs = build_long_history(10);
    // Mark the last 4 user messages with sentinel content we can check.
    for (int i = 0; i < 4; ++i) {
        msgs[msgs.size() - 1 - 2 * i].content_text =
            "tail-sentinel-" + std::to_string(i);
    }
    auto out = c.compress(msgs, /*current=*/90000, /*max=*/100000);
    int found = 0;
    for (const auto& m : out) {
        if (m.content_text.rfind("tail-sentinel-", 0) == 0) ++found;
    }
    EXPECT_EQ(found, 4);
}

TEST(ContextCompressor, OnSessionResetIsIdempotent) {
    ScriptedClient sc;
    ContextCompressor c(&sc, "gpt-aux");
    c.on_session_reset();
    c.on_session_reset();
    EXPECT_EQ(c.compression_count(), 0);
}

TEST(ContextCompressor, UpdateModelStoresContextLength) {
    ScriptedClient sc;
    ContextCompressor c(&sc, "gpt-aux");
    hermes::llm::ModelMetadata meta;
    meta.context_length = 32000;
    c.update_model(meta);
    // No exception, no public getter — but a follow-up compress() call
    // must still work.
    auto out =
        c.compress(build_long_history(3), /*current=*/10, /*max=*/100000);
    EXPECT_FALSE(out.empty());
}
