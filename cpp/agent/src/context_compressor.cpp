#include "hermes/agent/context_compressor.hpp"

#include "hermes/agent/context_compressor_depth.hpp"
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
namespace depth = hermes::agent::compressor_depth;

std::string role_string(Role r) {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

// Convert hermes::llm::Message -> compressor_depth::Message so the pure
// helpers can operate without knowing about provider serialisation. We
// preserve tool_calls (id/name/args) and tool_call_id so boundary
// alignment + sanitization stay accurate. Reasoning / content_blocks
// are approximated by flattening textual content — the depth helpers
// only need enough signal for token estimation and role decisions.
depth::Message to_depth(const Message& m) {
    depth::Message d;
    d.role = role_string(m.role);
    // Prefer the scalar content_text; fall back to concatenating text
    // content_blocks so we don't under-count tokens for Anthropic-style
    // multi-part messages.
    d.content = m.content_text;
    if (d.content.empty()) {
        for (const auto& b : m.content_blocks) {
            if (b.type == "text") {
                if (!d.content.empty()) d.content.push_back('\n');
                d.content += b.text;
            }
        }
    }
    if (m.tool_call_id) d.tool_call_id = *m.tool_call_id;
    for (const auto& tc : m.tool_calls) {
        depth::ToolCall dtc;
        dtc.id = tc.id;
        dtc.name = tc.name;
        dtc.arguments = tc.arguments.is_null() ? "" : tc.arguments.dump();
        d.tool_calls.push_back(std::move(dtc));
    }
    return d;
}

std::vector<depth::Message> to_depth_vec(const std::vector<Message>& v) {
    std::vector<depth::Message> out;
    out.reserve(v.size());
    for (const auto& m : v) out.push_back(to_depth(m));
    return out;
}

// Apply content-only mutations from a depth-sanitised message back
// onto the original Message. Used after prune_old_tool_results which
// rewrites content strings but does not touch other fields.
void apply_content_back(Message& dst, const depth::Message& src) {
    dst.content_text = src.content;
}

}  // namespace

ContextCompressor::ContextCompressor(hermes::llm::LlmClient* summarizer_client,
                                     std::string summarizer_model,
                                     CompressionOptions opts)
    : summarizer_(summarizer_client),
      summarizer_model_(std::move(summarizer_model)),
      opts_(std::move(opts)) {}

// -----------------------------------------------------------------------
// Private pipeline stages
// -----------------------------------------------------------------------

std::vector<Message> ContextCompressor::prune_old_tool_results(
    std::vector<Message> messages) const {
    if (messages.empty()) return messages;
    auto depth_vec = to_depth_vec(messages);
    depth::PruneReport rep;
    auto pruned = depth::prune_old_tool_results(
        std::move(depth_vec),
        static_cast<std::size_t>(std::max(0, opts_.protected_tail_turns)),
        opts_.protected_tail_tokens,
        &rep);
    // Only tool-role content is rewritten; keep other fields intact.
    for (std::size_t i = 0; i < messages.size() && i < pruned.size(); ++i) {
        if (messages[i].role == Role::Tool) {
            apply_content_back(messages[i], pruned[i]);
        }
    }
    return messages;
}

std::size_t ContextCompressor::align_cut_forward(
    const std::vector<Message>& messages, std::size_t idx) const {
    auto dv = to_depth_vec(messages);
    return depth::align_boundary_forward(dv, idx);
}

std::size_t ContextCompressor::align_cut_backward(
    const std::vector<Message>& messages, std::size_t idx) const {
    auto dv = to_depth_vec(messages);
    return depth::align_boundary_backward(dv, idx);
}

std::size_t ContextCompressor::find_tail_cut_by_tokens(
    const std::vector<Message>& messages, std::size_t head_end) const {
    auto dv = to_depth_vec(messages);
    return depth::find_tail_cut_by_tokens(
        dv, head_end, opts_.protected_tail_tokens);
}

std::string ContextCompressor::serialize_for_summary(
    const std::vector<Message>& turns) const {
    auto dv = to_depth_vec(turns);
    return depth::serialize_for_summary(dv);
}

std::optional<std::string> ContextCompressor::generate_summary(
    const std::vector<Message>& turns) {
    if (!summarizer_) return std::nullopt;
    if (turns.empty()) return std::nullopt;

    auto dv = to_depth_vec(turns);
    // Scale the summariser output cap with the content being
    // compressed. The Python version uses a context-length-derived
    // ceiling; here we take the depth helper's default ceiling
    // (kSummaryTokensCeiling) unless the model's context length was
    // plumbed in via update_model.
    depth::SummaryBudgetConfig cfg;
    if (current_model_context_ > 0) {
        cfg.max_summary_tokens =
            depth::derive_max_summary_tokens(current_model_context_);
    }
    std::int64_t budget = depth::compute_summary_budget(dv, cfg);

    std::string content_to_summarize = depth::serialize_for_summary(dv);

    std::string prompt;
    if (previous_summary_ && !previous_summary_->empty()) {
        prompt =
            "You are updating a context compaction summary. A previous "
            "compaction produced the summary below. New conversation "
            "turns have occurred since then and need to be incorporated.\n\n"
            "PREVIOUS SUMMARY:\n" + *previous_summary_ + "\n\n"
            "NEW TURNS TO INCORPORATE:\n" + content_to_summarize + "\n\n"
            "Update the summary. PRESERVE existing information that is still "
            "relevant. ADD new progress. Move items from \"In Progress\" to "
            "\"Done\" when completed. Remove information only if clearly "
            "obsolete.\n\n"
            "Use this exact structure:\n"
            "## Goal\n## Constraints & Preferences\n"
            "## Progress\n### Done\n### In Progress\n### Blocked\n"
            "## Key Decisions\n## Relevant Files\n## Next Steps\n"
            "## Critical Context\n## Tools & Patterns\n\n"
            "Target ~" + std::to_string(budget) + " tokens. Be specific — "
            "include file paths, command outputs, error messages, and "
            "concrete values. Write only the summary body.";
    } else {
        prompt =
            "Create a structured handoff summary for a later assistant "
            "that will continue this conversation after earlier turns are "
            "compacted.\n\n"
            "TURNS TO SUMMARIZE:\n" + content_to_summarize + "\n\n"
            "Use this exact structure:\n"
            "## Goal\n## Constraints & Preferences\n"
            "## Progress\n### Done\n### In Progress\n### Blocked\n"
            "## Key Decisions\n## Relevant Files\n## Next Steps\n"
            "## Critical Context\n## Tools & Patterns\n\n"
            "Target ~" + std::to_string(budget) + " tokens. Be specific — "
            "include file paths, command outputs, error messages, and "
            "concrete values. Write only the summary body.";
    }

    using hermes::llm::CompletionRequest;
    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content_text = std::move(prompt);

    CompletionRequest req;
    req.model = summarizer_model_;
    req.messages = {std::move(user_msg)};
    req.temperature = 0.2;
    req.max_tokens = static_cast<int>(std::min<std::int64_t>(
        budget * 2, static_cast<std::int64_t>(1 << 30)));

    std::string body;
    try {
        auto resp = summarizer_->complete(req);
        body = resp.assistant_message.content_text;
        if (body.empty() && !resp.assistant_message.content_blocks.empty()) {
            for (const auto& b : resp.assistant_message.content_blocks) {
                if (b.type == "text") { body = b.text; break; }
            }
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    if (body.empty()) return std::nullopt;
    // Persist the raw (unprefixed) body so iterative updates can build
    // on it without the prefix noise.
    previous_summary_ = body;
    return depth::with_summary_prefix(body);
}

// -----------------------------------------------------------------------
// Main entry point
// -----------------------------------------------------------------------

std::vector<Message> ContextCompressor::compress(
    std::vector<Message> messages,
    int64_t current_tokens,
    int64_t max_tokens) {
    if (max_tokens <= 0) max_tokens = 1;
    const double usage = static_cast<double>(current_tokens) / max_tokens;
    if (usage < opts_.trigger_threshold) return messages;
    if (!summarizer_) return messages;
    if (!depth::can_compress(messages.size(), opts_.protect_first_n)) {
        return messages;
    }

    // Phase 1: prune old tool results (cheap, no LLM).
    messages = prune_old_tool_results(std::move(messages));

    // Phase 2: determine boundaries.
    std::size_t head_end = static_cast<std::size_t>(
        std::max(0, opts_.protect_first_n));
    if (head_end > messages.size()) head_end = messages.size();
    // Respect the classic "system + first full turn" head if the
    // caller didn't configure protect_first_n explicitly.  We keep the
    // max of the configured first_n and the index just past the first
    // assistant reply.
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == Role::Assistant) {
            head_end = std::max(head_end, i + 1);
            break;
        }
    }
    head_end = align_cut_forward(messages, head_end);
    if (head_end > messages.size()) head_end = messages.size();

    std::size_t tail_start = find_tail_cut_by_tokens(messages, head_end);
    if (tail_start <= head_end) return messages;

    std::vector<Message> middle(messages.begin() + head_end,
                                messages.begin() + tail_start);
    if (middle.empty()) return messages;

    // Phase 3: generate the structured summary.
    std::optional<std::string> summary = generate_summary(middle);

    // On LLM failure fall back to a static marker so the model still
    // knows context was removed.
    if (!summary) {
        std::string marker =
            std::string(depth::kSummaryPrefix) + "\n"
            "Summary generation was unavailable. " +
            std::to_string(middle.size()) +
            " conversation turns were removed to free context space but "
            "could not be summarised. Continue based on the recent messages "
            "below and the current state of any files or resources.";
        summary = std::move(marker);
    }

    // Phase 4: assemble compressed message list.
    std::string head_role = head_end == 0
        ? std::string("user")
        : role_string(messages[head_end - 1].role);
    std::string tail_role = tail_start >= messages.size()
        ? std::string("user")
        : role_string(messages[tail_start].role);
    depth::SummaryRoleResult role_pick =
        depth::pick_summary_role(head_role, tail_role);

    std::vector<Message> compressed;
    compressed.reserve(head_end + 1 + (messages.size() - tail_start));
    for (std::size_t i = 0; i < head_end; ++i) {
        compressed.push_back(std::move(messages[i]));
    }

    bool merge_into_tail = role_pick.merge_into_tail;
    if (!merge_into_tail) {
        Message sm;
        sm.role = (role_pick.role == "assistant") ? Role::Assistant : Role::User;
        sm.content_text = *summary;
        compressed.push_back(std::move(sm));
    }

    for (std::size_t i = tail_start; i < messages.size(); ++i) {
        Message m = std::move(messages[i]);
        if (merge_into_tail && i == tail_start) {
            std::string original = std::move(m.content_text);
            m.content_text = *summary + "\n\n" + original;
            merge_into_tail = false;
        }
        compressed.push_back(std::move(m));
    }

    // Phase 5: sanitize tool_call / tool_result pairs. We only need the
    // bookkeeping here — the full mutation is mirrored back onto the
    // Message vector so we don't lose non-content fields (reasoning,
    // cache markers).
    {
        auto dv = to_depth_vec(compressed);
        depth::SanitizeReport rep;
        auto cleaned = depth::sanitize_tool_pairs(std::move(dv), &rep);
        if (rep.orphan_results_removed > 0 || rep.stubs_inserted > 0) {
            // Re-materialise from the cleaned depth vector. When the
            // helper inserts stub tool messages we create fresh Message
            // entries to match; otherwise we index by role+tool_call_id
            // to line up with existing objects.
            std::vector<Message> rebuilt;
            rebuilt.reserve(cleaned.size());
            std::size_t src_idx = 0;
            for (const auto& dm : cleaned) {
                // Try to find a corresponding original message forward
                // from src_idx that matches role + tool_call_id +
                // content. If found, move it; else synthesise a stub.
                bool matched = false;
                for (std::size_t j = src_idx; j < compressed.size(); ++j) {
                    if (role_string(compressed[j].role) != dm.role) continue;
                    if (dm.role == "tool") {
                        if (compressed[j].tool_call_id.value_or("") != dm.tool_call_id) continue;
                    }
                    rebuilt.push_back(std::move(compressed[j]));
                    src_idx = j + 1;
                    matched = true;
                    break;
                }
                if (!matched) {
                    Message stub;
                    if (dm.role == "tool") {
                        stub.role = Role::Tool;
                        stub.tool_call_id = dm.tool_call_id;
                    } else if (dm.role == "assistant") {
                        stub.role = Role::Assistant;
                    } else if (dm.role == "system") {
                        stub.role = Role::System;
                    } else {
                        stub.role = Role::User;
                    }
                    stub.content_text = dm.content;
                    rebuilt.push_back(std::move(stub));
                }
            }
            compressed = std::move(rebuilt);
        }
    }

    ++this->ContextEngine::compression_count;
    return compressed;
}

void ContextCompressor::on_session_reset() {
    this->ContextEngine::compression_count = 0;
    previous_summary_.reset();
}

void ContextCompressor::update_model(const hermes::llm::ModelMetadata& meta) {
    current_model_context_ = meta.context_length;
    this->ContextEngine::context_length = meta.context_length;
}

}  // namespace hermes::agent
