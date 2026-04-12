#include "hermes/batch/trajectory_compressor.hpp"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace hermes::batch {
namespace {

TEST(TrajectoryCompressorTest, BelowTargetNoCompression) {
    CompressionConfig config;
    config.target_tokens = 100000;  // very high target

    TrajectoryCompressor compressor(config);

    nlohmann::json messages = nlohmann::json::array({
        {{"role", "user"}, {"content", "hello"}},
        {{"role", "assistant"}, {"content", "hi"}},
    });

    auto result = compressor.compress(messages);
    EXPECT_FALSE(result.was_compressed);
    EXPECT_EQ(result.messages.size(), 2u);
    EXPECT_EQ(result.original_tokens, result.compressed_tokens);
}

TEST(TrajectoryCompressorTest, AboveTargetCompressed) {
    CompressionConfig config;
    config.target_tokens = 1;  // impossibly low target — forces compression
    config.protected_head_turns = 1;
    config.protected_tail_turns = 1;

    TrajectoryCompressor compressor(config);

    nlohmann::json messages = nlohmann::json::array({
        {{"role", "user"}, {"content", "first message"}},
        {{"role", "assistant"}, {"content", "middle reply that is long"}},
        {{"role", "user"}, {"content", "another middle message"}},
        {{"role", "assistant"}, {"content", "yet another reply"}},
        {{"role", "user"}, {"content", "last message"}},
    });

    auto result = compressor.compress(messages);
    EXPECT_TRUE(result.was_compressed);
    EXPECT_LT(result.messages.size(), messages.size());
}

TEST(TrajectoryCompressorTest, ProtectedHeadTailPreserved) {
    CompressionConfig config;
    config.target_tokens = 1;
    config.protected_head_turns = 2;
    config.protected_tail_turns = 2;

    TrajectoryCompressor compressor(config);

    nlohmann::json messages = nlohmann::json::array({
        {{"role", "user"}, {"content", "head 1"}},
        {{"role", "assistant"}, {"content", "head 2"}},
        {{"role", "user"}, {"content", "middle 1"}},
        {{"role", "assistant"}, {"content", "middle 2"}},
        {{"role", "user"}, {"content", "middle 3"}},
        {{"role", "assistant"}, {"content", "tail 1"}},
        {{"role", "user"}, {"content", "tail 2"}},
    });

    auto result = compressor.compress(messages);
    EXPECT_TRUE(result.was_compressed);

    // First message should be preserved (head)
    EXPECT_EQ(result.messages[0]["content"], "head 1");
    EXPECT_EQ(result.messages[1]["content"], "head 2");

    // Last two messages should be preserved (tail)
    auto sz = result.messages.size();
    EXPECT_EQ(result.messages[sz - 1]["content"], "tail 2");
    EXPECT_EQ(result.messages[sz - 2]["content"], "tail 1");
}

TEST(TrajectoryCompressorTest, CompressFileCreatesOutput) {
    auto tmp_dir = std::filesystem::temp_directory_path() /
                   ("hermes_tc_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp_dir);

    auto input_path = tmp_dir / "input.jsonl";
    auto output_path = tmp_dir / "output.jsonl";

    // Write input JSONL
    {
        std::ofstream out(input_path);
        nlohmann::json entry;
        entry["messages"] = nlohmann::json::array({
            {{"role", "user"}, {"content", "hello"}},
            {{"role", "assistant"}, {"content", "hi"}},
        });
        out << entry.dump() << "\n";
    }

    CompressionConfig config;
    config.target_tokens = 100000;
    TrajectoryCompressor compressor(config);
    compressor.compress_file(input_path, output_path);

    ASSERT_TRUE(std::filesystem::exists(output_path));

    // Read and verify output
    std::ifstream in(output_path);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    auto result = nlohmann::json::parse(line);
    EXPECT_TRUE(result.contains("messages"));
    EXPECT_TRUE(result.contains("was_compressed"));

    std::filesystem::remove_all(tmp_dir);
}

}  // namespace
}  // namespace hermes::batch
