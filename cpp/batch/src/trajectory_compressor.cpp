#include "hermes/batch/trajectory_compressor.hpp"

#include <fstream>
#include <string>

namespace hermes::batch {

TrajectoryCompressor::TrajectoryCompressor(CompressionConfig config,
                                           hermes::llm::LlmClient* client)
    : config_(std::move(config)), client_(client) {}

CompressedTrajectory TrajectoryCompressor::compress(
    const nlohmann::json& messages) {
    CompressedTrajectory result;
    result.messages = messages;

    if (!messages.is_array()) {
        result.original_tokens = 0;
        result.compressed_tokens = 0;
        result.was_compressed = false;
        return result;
    }

    // Estimate tokens: ~4 chars per token
    auto estimate_tokens = [](const nlohmann::json& msgs) -> int64_t {
        return static_cast<int64_t>(msgs.dump().size()) / 4;
    };

    result.original_tokens = estimate_tokens(messages);

    if (result.original_tokens <= config_.target_tokens) {
        result.compressed_tokens = result.original_tokens;
        result.was_compressed = false;
        return result;
    }

    // Compress: keep protected head and tail, summarize middle
    auto total = static_cast<int>(messages.size());
    int head = std::min(config_.protected_head_turns, total);
    int tail = std::min(config_.protected_tail_turns, total - head);

    nlohmann::json compressed = nlohmann::json::array();

    // Copy head
    for (int i = 0; i < head; ++i) {
        compressed.push_back(messages[static_cast<size_t>(i)]);
    }

    // Summarize middle section
    int middle_start = head;
    int middle_end = total - tail;
    if (middle_start < middle_end) {
        nlohmann::json summary_msg;
        summary_msg["role"] = "system";

        if (client_) {
            // Use LLM to summarize middle turns
            std::string middle_text;
            for (int i = middle_start; i < middle_end; ++i) {
                auto& msg = messages[static_cast<size_t>(i)];
                middle_text += msg.dump() + "\n";
            }
            hermes::llm::CompletionRequest req;
            req.model = config_.model;
            hermes::llm::Message user_msg;
            user_msg.role = hermes::llm::Role::User;
            user_msg.content_text =
                "Summarize these conversation turns concisely:\n" +
                middle_text;
            req.messages.push_back(user_msg);
            auto resp = client_->complete(req);
            auto summary_text = resp.assistant_message.content_text;
            summary_msg["content"] =
                "[Compressed " + std::to_string(middle_end - middle_start) +
                " turns] " +
                (summary_text.empty() ? "Summary unavailable." : summary_text);
        } else {
            summary_msg["content"] =
                "[Compressed " + std::to_string(middle_end - middle_start) +
                " middle turns]";
        }

        compressed.push_back(summary_msg);
    }

    // Copy tail
    for (int i = total - tail; i < total; ++i) {
        compressed.push_back(messages[static_cast<size_t>(i)]);
    }

    result.messages = compressed;
    result.compressed_tokens = estimate_tokens(compressed);
    result.was_compressed = true;
    return result;
}

void TrajectoryCompressor::compress_file(const std::filesystem::path& input,
                                         const std::filesystem::path& output) {
    std::ifstream in(input);
    std::ofstream out(output);

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto entry = nlohmann::json::parse(line);
        auto messages = entry.value("messages", nlohmann::json::array());
        auto compressed = compress(messages);
        entry["messages"] = compressed.messages;
        entry["original_tokens"] = compressed.original_tokens;
        entry["compressed_tokens"] = compressed.compressed_tokens;
        entry["was_compressed"] = compressed.was_compressed;
        out << entry.dump() << "\n";
    }
}

}  // namespace hermes::batch
