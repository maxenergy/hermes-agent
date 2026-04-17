// End-to-end integration tests for the ContextCompressor pipeline — exercises
// the new Python-aligned wiring (prune → align → find_tail_cut → serialize →
// generate_summary → sanitize → pick_summary_role) via the public compress()
// entry point.

#include "hermes/agent/context_compressor.hpp"
#include "hermes/agent/context_compressor_depth.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using hermes::agent::CompressionOptions;
using hermes::agent::ContextCompressor;
namespace depth = hermes::agent::compressor_depth;
using hermes::llm::CompletionRequest;
using hermes::llm::CompletionResponse;
using hermes::llm::LlmClient;
using hermes::llm::Message;
using hermes::llm::Role;
using hermes::llm::ToolCall;

namespace {

class FakeSummarizer : public LlmClient {
public:
    FakeSummarizer() = default;
    void enqueue(std::string content) {
        Message m;
        m.role = Role::Assistant;
        m.content_text = std::move(content);
        scripts_.push_back(std::move(m));
    }
    CompletionResponse complete(const CompletionRequest& req) override {
        ++call_count_;
        last_prompt_.clear();
        if (!req.messages.empty()) last_prompt_ = req.messages.back().content_text;
        if (scripts_.empty()) throw std::runtime_error("no scripted response");
        CompletionResponse out;
        out.assistant_message = scripts_.front();
        scripts_.erase(scripts_.begin());
        out.finish_reason = "stop";
        return out;
    }
    std::string provider_name() const override { return "fake"; }

    int call_count() const { return call_count_; }
    const std::string& last_prompt() const { return last_prompt_; }

private:
    std::vector<Message> scripts_;
    std::string last_prompt_;
    int call_count_ = 0;
};

Message make(Role r, std::string txt) {
    Message m;
    m.role = r;
    m.content_text = std::move(txt);
    return m;
}

Message make_tool_result(std::string tool_call_id, std::string txt) {
    Message m;
    m.role = Role::Tool;
    m.tool_call_id = std::move(tool_call_id);
    m.content_text = std::move(txt);
    return m;
}

Message make_asst_with_tc(std::string txt, std::string tc_id, std::string name) {
    Message m;
    m.role = Role::Assistant;
    m.content_text = std::move(txt);
    ToolCall tc;
    tc.id = std::move(tc_id);
    tc.name = std::move(name);
    tc.arguments = nlohmann::json::object();
    m.tool_calls.push_back(std::move(tc));
    return m;
}

}  // namespace

// ---------------------------------------------------------------------
// 1. prune_old_tool_results — large tool results in the head get
//    replaced with a placeholder, tail results survive.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, PruneReplacesOldSubstantialToolResults) {
    FakeSummarizer fake;
    fake.enqueue("ok");
    CompressionOptions opts;
    opts.protected_tail_tokens = 500;
    opts.protect_first_n = 3;
    ContextCompressor c(&fake, "fake-model", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    // 10 turns: each alternating user + assistant-with-tc + tool-result.
    for (int i = 0; i < 10; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i)));
        msgs.push_back(make_asst_with_tc("a" + std::to_string(i),
                                         "call-" + std::to_string(i),
                                         "fn"));
        // 300-char tool body so it beats the 200-char prune floor.
        msgs.push_back(make_tool_result("call-" + std::to_string(i),
                                        std::string(300, 'x')));
    }

    auto out = c.compress(msgs, /*current=*/9000, /*max=*/10000);

    // At least one tool-role message in the head portion was reduced to
    // the prune placeholder (which is much shorter than 300 chars).
    int placeholders = 0;
    for (const auto& m : out) {
        if (m.role == Role::Tool &&
            m.content_text == std::string(depth::kPrunedToolPlaceholder)) {
            ++placeholders;
        }
    }
    EXPECT_GT(placeholders, 0);
}

// ---------------------------------------------------------------------
// 2. align_boundary_backward — the tail cut never splits an
//    assistant+tool_result group.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, TailCutDoesNotSplitToolGroup) {
    FakeSummarizer fake;
    fake.enqueue("ok");
    CompressionOptions opts;
    opts.protected_tail_tokens = 60;  // budget ~= 3 small messages
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake-model", opts);

    // 12 messages where a tool_result sits just on the cut boundary.
    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 4; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i)));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i)));
    }
    msgs.push_back(make(Role::User, "u4"));
    msgs.push_back(make_asst_with_tc("a4", "c4", "fn"));
    msgs.push_back(make_tool_result("c4", "result4"));
    msgs.push_back(make(Role::Assistant, "a5"));

    auto out = c.compress(msgs, /*current=*/9000, /*max=*/10000);

    // Scan for orphaned tool_results: any tool message whose
    // tool_call_id is not backed by an assistant tool_call in out.
    std::set<std::string> live_ids;
    for (const auto& m : out) {
        if (m.role == Role::Assistant) {
            for (const auto& tc : m.tool_calls) live_ids.insert(tc.id);
        }
    }
    for (const auto& m : out) {
        if (m.role == Role::Tool) {
            const std::string& cid = m.tool_call_id.value_or("");
            if (!cid.empty()) {
                EXPECT_TRUE(live_ids.count(cid) == 1)
                    << "orphan tool_result with id=" << cid;
            }
        }
    }
}

// ---------------------------------------------------------------------
// 3. find_tail_cut_by_tokens — tail budget controls how many messages
//    survive the cut. Larger budget (but still below the total
//    conversation size) keeps more tail messages intact.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, TailBudgetControlsTailSize) {
    FakeSummarizer fake_a;
    fake_a.enqueue("ok");
    FakeSummarizer fake_b;
    fake_b.enqueue("ok");

    CompressionOptions opts_tight;
    opts_tight.protected_tail_tokens = 20;   // tight — ~min 3 messages
    opts_tight.protect_first_n = 1;
    CompressionOptions opts_medium;
    // Roomy enough to keep ~6-8 tail messages but not everything (each
    // msg ≈ 30 tokens, so 300 tokens → ~10 messages survive).
    opts_medium.protected_tail_tokens = 300;
    opts_medium.protect_first_n = 1;

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 20; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i) +
                                            std::string(80, 'x')));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i) +
                                                 std::string(80, 'y')));
    }

    ContextCompressor tight(&fake_a, "fake", opts_tight);
    auto tight_out = tight.compress(msgs, /*cur=*/9000, /*max=*/10000);

    ContextCompressor medium(&fake_b, "fake", opts_medium);
    auto medium_out = medium.compress(msgs, /*cur=*/9000, /*max=*/10000);

    // Medium budget preserves more tail messages than the tight budget.
    EXPECT_LT(tight_out.size(), medium_out.size());
}

// ---------------------------------------------------------------------
// 4. generate_summary — fake summariser returns canned body, which is
//    wrapped with the [CONTEXT COMPACTION] prefix and injected into
//    the compressed output.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, GenerateSummaryInjectsPrefixedBody) {
    FakeSummarizer fake;
    fake.enqueue("## Goal\nship it\n## Progress\nhalf done");
    CompressionOptions opts;
    opts.protected_tail_tokens = 100;
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 8; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i) +
                                            std::string(200, 'x')));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i) +
                                                 std::string(200, 'y')));
    }

    auto out = c.compress(msgs, /*cur=*/9000, /*max=*/10000);
    EXPECT_EQ(fake.call_count(), 1);

    bool found_prefix = false;
    bool found_body = false;
    for (const auto& m : out) {
        if (m.content_text.find("[CONTEXT COMPACTION]") != std::string::npos) {
            found_prefix = true;
        }
        if (m.content_text.find("ship it") != std::string::npos) {
            found_body = true;
        }
    }
    EXPECT_TRUE(found_prefix);
    EXPECT_TRUE(found_body);
}

// ---------------------------------------------------------------------
// 5. Iterative summary — second compress() call uses previous_summary()
//    in the prompt.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, IterativeSummaryFeedsPreviousBodyIntoPrompt) {
    FakeSummarizer fake;
    fake.enqueue("first summary body");
    fake.enqueue("second summary body");
    CompressionOptions opts;
    opts.protected_tail_tokens = 100;
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 8; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i) +
                                            std::string(200, 'x')));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i) +
                                                 std::string(200, 'y')));
    }

    auto out1 = c.compress(msgs, /*cur=*/9000, /*max=*/10000);
    ASSERT_TRUE(c.previous_summary().has_value());
    EXPECT_EQ(*c.previous_summary(), "first summary body");

    // Second compression: the new prompt should quote the previous body.
    auto out2 = c.compress(out1, /*cur=*/9000, /*max=*/10000);
    EXPECT_NE(fake.last_prompt().find("first summary body"), std::string::npos)
        << "iterative prompt should embed previous summary";
    EXPECT_EQ(*c.previous_summary(), "second summary body");
}

// ---------------------------------------------------------------------
// 6. Summariser failure — LLM throwing yields a static fallback marker
//    rather than losing the context silently.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, FallbackMarkerOnSummariserFailure) {
    FakeSummarizer fake;  // no scripted responses → will throw
    CompressionOptions opts;
    opts.protected_tail_tokens = 100;
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 8; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i) +
                                            std::string(200, 'x')));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i) +
                                                 std::string(200, 'y')));
    }

    auto out = c.compress(msgs, /*cur=*/9000, /*max=*/10000);
    bool found_fallback = false;
    for (const auto& m : out) {
        if (m.content_text.find("Summary generation was unavailable") !=
            std::string::npos) {
            found_fallback = true;
            break;
        }
    }
    EXPECT_TRUE(found_fallback);
    EXPECT_FALSE(c.previous_summary().has_value())
        << "failed summaries must not poison iterative state";
}

// ---------------------------------------------------------------------
// 7. serialize_for_summary — the summariser prompt includes TOOL RESULT
//    markers so the compressor can reason about tool outputs.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, SerializerMentionsToolBlocks) {
    FakeSummarizer fake;
    fake.enqueue("ok");
    CompressionOptions opts;
    opts.protected_tail_tokens = 60;
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 5; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i)));
        msgs.push_back(make_asst_with_tc("asst" + std::to_string(i),
                                         "c" + std::to_string(i), "fn"));
        msgs.push_back(make_tool_result("c" + std::to_string(i),
                                        "toolout_" + std::to_string(i)));
    }
    msgs.push_back(make(Role::User, "ulast"));
    msgs.push_back(make(Role::Assistant, "alast"));

    c.compress(msgs, /*cur=*/9000, /*max=*/10000);
    EXPECT_EQ(fake.call_count(), 1);
    EXPECT_NE(fake.last_prompt().find("[ASSISTANT]"), std::string::npos);
    EXPECT_NE(fake.last_prompt().find("TOOL RESULT"), std::string::npos);
}

// ---------------------------------------------------------------------
// 8. on_session_reset — clears the iterative summary.
// ---------------------------------------------------------------------

TEST(ContextCompressorPipeline, ResetClearsPreviousSummary) {
    FakeSummarizer fake;
    fake.enqueue("first");
    CompressionOptions opts;
    opts.protected_tail_tokens = 100;
    opts.protect_first_n = 1;
    ContextCompressor c(&fake, "fake", opts);

    std::vector<Message> msgs;
    msgs.push_back(make(Role::System, "sys"));
    for (int i = 0; i < 8; ++i) {
        msgs.push_back(make(Role::User, "u" + std::to_string(i) +
                                            std::string(200, 'x')));
        msgs.push_back(make(Role::Assistant, "a" + std::to_string(i) +
                                                 std::string(200, 'y')));
    }
    c.compress(msgs, /*cur=*/9000, /*max=*/10000);
    ASSERT_TRUE(c.previous_summary().has_value());

    c.on_session_reset();
    EXPECT_FALSE(c.previous_summary().has_value());
    EXPECT_EQ(c.compression_count(), 0);
}
