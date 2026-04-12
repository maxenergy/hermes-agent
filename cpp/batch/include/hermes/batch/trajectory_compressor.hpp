// TrajectoryCompressor — shrinks long conversation trajectories to a token budget.
#pragma once

#include "hermes/llm/llm_client.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace hermes::batch {

struct CompressionConfig {
    int64_t target_tokens = 15000;
    int protected_head_turns = 1;
    int protected_tail_turns = 4;
    std::string model = "openrouter/google/gemini-2.0-flash-001";
};

struct CompressedTrajectory {
    nlohmann::json messages;  // OpenAI format array
    int64_t original_tokens;
    int64_t compressed_tokens;
    bool was_compressed;
};

class TrajectoryCompressor {
public:
    explicit TrajectoryCompressor(CompressionConfig config,
                                  hermes::llm::LlmClient* client = nullptr);

    CompressedTrajectory compress(const nlohmann::json& messages);

    // Batch compress a JSONL file
    void compress_file(const std::filesystem::path& input,
                       const std::filesystem::path& output);

private:
    CompressionConfig config_;
    hermes::llm::LlmClient* client_;  // for LLM summarization of middle turns
};

}  // namespace hermes::batch
