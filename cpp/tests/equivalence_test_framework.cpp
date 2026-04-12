#include "equivalence_test_framework.hpp"

#include "hermes/agent/ai_agent.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hermes::tests {

std::vector<EquivalenceCase> load_equivalence_cases(const std::filesystem::path& jsonl) {
    std::vector<EquivalenceCase> cases;
    std::ifstream ifs(jsonl);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open equivalence cases file: " + jsonl.string());
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto j = nlohmann::json::parse(line);
        EquivalenceCase tc;
        tc.name = j.value("name", "unnamed");
        tc.input = j.value("input", nlohmann::json::object());
        tc.expected_output = j.value("expected_output", nlohmann::json::object());
        cases.push_back(std::move(tc));
    }
    return cases;
}

namespace {

hermes::llm::HttpTransport::Response make_openai_response(const std::string& text) {
    hermes::llm::HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = nlohmann::json{
        {"id", "chatcmpl-eq"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"finish_reason", "stop"},
            {"message", {
                {"role", "assistant"},
                {"content", text},
            }},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
    }.dump();
    return resp;
}

hermes::llm::HttpTransport::Response make_tool_call_response(
    const std::string& tool_name,
    const nlohmann::json& args,
    const std::string& call_id = "call_eq") {
    hermes::llm::HttpTransport::Response resp;
    resp.status_code = 200;
    resp.body = nlohmann::json{
        {"id", "chatcmpl-eq-tc"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", nlohmann::json::array({{
                    {"id", call_id},
                    {"type", "function"},
                    {"function", {
                        {"name", tool_name},
                        {"arguments", args.dump()},
                    }},
                }})},
            }},
        }})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
    }.dump();
    return resp;
}

}  // namespace

EquivalenceResult run_equivalence(const EquivalenceCase& tc) {
    using namespace hermes::agent;
    using namespace hermes::llm;

    FakeHttpTransport fake;
    OpenAIClient client(&fake, "sk-test");

    // Enqueue model responses from the fixture.
    if (tc.input.contains("model_responses")) {
        for (auto& mr : tc.input["model_responses"]) {
            if (mr.contains("tool_call")) {
                auto tc_name = mr["tool_call"].value("name", "");
                auto tc_args = mr["tool_call"].value("arguments", nlohmann::json::object());
                fake.enqueue_response(make_tool_call_response(tc_name, tc_args));
            } else {
                auto text = mr.value("text", "");
                fake.enqueue_response(make_openai_response(text));
            }
        }
    }

    PromptBuilder builder;
    AgentConfig config;
    config.model = "gpt-4o";
    config.max_iterations = 90;
    config.max_retries = 0;

    // Set up a simple dispatcher that returns tool_seed if provided.
    auto tool_seed = tc.input.value("tool_seed", nlohmann::json::object());
    ToolDispatcher dispatcher = [&](const std::string& name,
                                    const nlohmann::json&,
                                    const std::string&) -> std::string {
        if (tool_seed.contains(name)) {
            return tool_seed[name].dump();
        }
        return R"({"ok":true})";
    };

    auto agent = std::make_unique<AIAgent>(
        config, &client, nullptr, nullptr, nullptr, &builder,
        std::move(dispatcher), std::vector<ToolSchema>{}, AgentCallbacks{});
    agent->set_sleep_function([](std::chrono::milliseconds) {});

    auto prompt = tc.input.value("prompt", "hello");
    auto result = agent->run_conversation(prompt);

    // Compare against expected output.
    EquivalenceResult er;
    er.passed = true;

    if (tc.expected_output.contains("final_response_contains")) {
        auto needle = tc.expected_output["final_response_contains"].get<std::string>();
        if (result.final_response.find(needle) == std::string::npos) {
            er.passed = false;
            er.diff = "Expected response to contain '" + needle +
                      "', got: '" + result.final_response + "'";
            return er;
        }
    }

    if (tc.expected_output.contains("tool_calls")) {
        auto expected_count = tc.expected_output["tool_calls"].get<int>();
        int actual_tool_calls = 0;
        for (auto& m : result.messages) {
            if (!m.tool_calls.empty()) {
                actual_tool_calls += static_cast<int>(m.tool_calls.size());
            }
        }
        if (actual_tool_calls != expected_count) {
            er.passed = false;
            er.diff = "Expected " + std::to_string(expected_count) +
                      " tool calls, got " + std::to_string(actual_tool_calls);
            return er;
        }
    }

    if (tc.expected_output.contains("token_range")) {
        auto range = tc.expected_output["token_range"];
        auto min_tokens = range.value("min", static_cast<int64_t>(0));
        auto max_tokens = range.value("max", static_cast<int64_t>(999999));
        auto total = result.usage.input_tokens + result.usage.output_tokens;
        if (total < min_tokens || total > max_tokens) {
            er.passed = false;
            er.diff = "Token count " + std::to_string(total) +
                      " outside range [" + std::to_string(min_tokens) +
                      ", " + std::to_string(max_tokens) + "]";
            return er;
        }
    }

    return er;
}

}  // namespace hermes::tests
