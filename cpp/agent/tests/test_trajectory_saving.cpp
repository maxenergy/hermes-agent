// Tests that save_trajectories=true creates a JSONL file via AIAgent.
#include "hermes/agent/ai_agent.hpp"

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using hermes::llm::CompletionRequest;
using hermes::llm::CompletionResponse;
using hermes::llm::Message;
using hermes::llm::Role;

// Minimal LLM stub that returns a canned response.
class StubLlmClient : public hermes::llm::LlmClient {
public:
    CompletionResponse complete(const CompletionRequest& /*req*/) override {
        CompletionResponse resp;
        resp.assistant_message.role = Role::Assistant;
        resp.assistant_message.content_text = "trajectory test response";
        resp.finish_reason = "stop";
        return resp;
    }
    std::string provider_name() const override { return "stub"; }
};

}  // namespace

TEST(TrajectorySaving, CreatesJsonlFile) {
    auto tmp = std::filesystem::temp_directory_path() / "hermes_traj_test";
    std::filesystem::create_directories(tmp);

    // Point HERMES_HOME to the temp dir so trajectory goes there.
    auto old_home = std::getenv("HERMES_HOME");
    setenv("HERMES_HOME", tmp.c_str(), 1);

    auto traj_file = tmp / "trajectories" / "traj-session.jsonl";
    std::filesystem::remove(traj_file);

    StubLlmClient llm;
    hermes::agent::PromptBuilder pb;

    hermes::agent::AgentConfig config;
    config.save_trajectories = true;
    config.session_id = "traj-session";
    config.max_iterations = 5;

    hermes::agent::AIAgent agent(
        config, &llm, nullptr, nullptr, nullptr, &pb,
        nullptr, {}, {});

    auto result = agent.run_conversation("test trajectory saving");

    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(std::filesystem::exists(traj_file))
        << "Expected trajectory file at " << traj_file;

    if (std::filesystem::exists(traj_file)) {
        std::ifstream in(traj_file);
        std::string line;
        ASSERT_TRUE(std::getline(in, line));
        auto j = nlohmann::json::parse(line);
        EXPECT_TRUE(j.contains("conversations"));
        EXPECT_TRUE(j.contains("model"));
        EXPECT_TRUE(j["completed"].get<bool>());
    }

    // Cleanup.
    if (old_home) {
        setenv("HERMES_HOME", old_home, 1);
    } else {
        unsetenv("HERMES_HOME");
    }
    std::filesystem::remove_all(tmp);
}
