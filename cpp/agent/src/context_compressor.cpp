#include "hermes/agent/context_compressor.hpp"

#include "hermes/llm/model_metadata.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace hermes::agent {

namespace {

using hermes::llm::Message;
using hermes::llm::Role;

// Walk forward from `start` until we have consumed the first complete
// turn (the run of messages until the first assistant reply).  Returns
// the index just past the end of that turn.
size_t end_of_first_turn(const std::vector<Message>& msgs, size_t start) {
    for (size_t i = start; i < msgs.size(); ++i) {
        if (msgs[i].role == Role::Assistant) return i + 1;
    }
    return msgs.size();
}

// Walk backwards from the end accumulating tail turns.  A "turn" is
// the run starting at a user message.  Stops once we have either
// `min_turns` complete turns or `min_tokens` worth of messages.
size_t start_of_protected_tail(const std::vector<Message>& msgs,
                               int min_turns,
                               int64_t min_tokens) {
    if (msgs.empty()) return 0;
    int turns = 0;
    int64_t tokens = 0;
    size_t cut = msgs.size();
    while (cut > 0) {
        size_t prev = cut - 1;
        const auto& m = msgs[prev];
        std::vector<Message> single = {m};
        tokens += hermes::llm::estimate_messages_tokens_rough(single);
        cut = prev;
        if (m.role == Role::User) {
            ++turns;
            if (turns >= min_turns && tokens >= min_tokens) {
                break;
            }
            if (turns >= min_turns * 2) {
                // Hard cap: never go beyond 2x the requested turn count.
                break;
            }
        }
    }
    return cut;
}

std::string render_summary(const CompressionOptions& opts,
                           const std::string& body) {
    // The body returned by the summariser is treated as Markdown.  We
    // wrap it with a stable header so the model knows the summary's
    // provenance.  If the summariser already returned a fully-formed
    // template fill, we still prepend the header.
    std::string out;
    out.reserve(body.size() + opts.summary_template.size());
    out += "## Compressed history summary\n";
    out += body;
    if (out.empty() || out.back() != '\n') out += '\n';
    return out;
}

std::string serialize_messages(const std::vector<Message>& msgs) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& m : msgs) {
        j.push_back(m.to_openai());
    }
    return j.dump();
}

}  // namespace

ContextCompressor::ContextCompressor(hermes::llm::LlmClient* summarizer_client,
                                     std::string summarizer_model,
                                     CompressionOptions opts)
    : summarizer_(summarizer_client),
      summarizer_model_(std::move(summarizer_model)),
      opts_(std::move(opts)) {}

std::vector<Message> ContextCompressor::compress(
    std::vector<Message> messages,
    int64_t current_tokens,
    int64_t max_tokens) {
    if (max_tokens <= 0) max_tokens = 1;
    const double usage = static_cast<double>(current_tokens) / max_tokens;
    if (usage < opts_.trigger_threshold) return messages;
    if (!summarizer_) return messages;

    // Identify the static head: system message (index 0 if Role::System)
    // plus the first complete turn.
    size_t head_end = 0;
    if (!messages.empty() && messages[0].role == Role::System) {
        head_end = 1;
    }
    head_end = end_of_first_turn(messages, head_end);
    if (head_end > messages.size()) head_end = messages.size();

    size_t tail_start = start_of_protected_tail(
        messages, opts_.protected_tail_turns, opts_.protected_tail_tokens);
    if (tail_start < head_end) tail_start = head_end;

    if (tail_start <= head_end) {
        // Nothing in the middle to summarise.
        return messages;
    }

    std::vector<Message> middle(messages.begin() + head_end,
                                messages.begin() + tail_start);
    if (middle.empty()) return messages;

    // Build the summarisation request.
    using hermes::llm::CompletionRequest;
    Message sys;
    sys.role = Role::System;
    sys.content_text =
        "You are a compression assistant. Summarise the following "
        "conversation slice into the fields Goal, Progress, Decisions, "
        "Files touched, Next steps. Be terse — no preamble.";

    Message user;
    user.role = Role::User;
    user.content_text = serialize_messages(middle);

    CompletionRequest req;
    req.model = summarizer_model_;
    req.messages = {std::move(sys), std::move(user)};
    req.temperature = 0.2;
    req.max_tokens = 1024;

    std::string summary_body;
    try {
        auto resp = summarizer_->complete(req);
        summary_body = resp.assistant_message.content_text;
        if (summary_body.empty() &&
            !resp.assistant_message.content_blocks.empty()) {
            for (const auto& b : resp.assistant_message.content_blocks) {
                if (b.type == "text") {
                    summary_body = b.text;
                    break;
                }
            }
        }
    } catch (const std::exception&) {
        // Compression failure: keep messages as-is.  The caller may
        // surface a warning.
        return messages;
    }
    if (summary_body.empty()) {
        summary_body = "(summary unavailable)";
    }

    Message summary_msg;
    summary_msg.role = Role::System;
    summary_msg.content_text = render_summary(opts_, summary_body);

    std::vector<Message> compressed;
    compressed.reserve(head_end + 1 + (messages.size() - tail_start));
    for (size_t i = 0; i < head_end; ++i) compressed.push_back(std::move(messages[i]));
    compressed.push_back(std::move(summary_msg));
    for (size_t i = tail_start; i < messages.size(); ++i) {
        compressed.push_back(std::move(messages[i]));
    }
    ++compression_count_;
    return compressed;
}

void ContextCompressor::on_session_reset() {
    compression_count_ = 0;
}

void ContextCompressor::update_model(const hermes::llm::ModelMetadata& meta) {
    current_model_context_ = meta.context_length;
}

}  // namespace hermes::agent
