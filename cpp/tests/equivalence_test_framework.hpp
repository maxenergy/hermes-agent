// Equivalence test framework — run deterministic agent scenarios from JSONL
// fixtures and compare outputs against expected results.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::tests {

struct EquivalenceCase {
    std::string name;
    nlohmann::json input;           // {prompt, tool_seed, model_responses}
    nlohmann::json expected_output; // {final_response_contains, tool_calls, token_range}
};

// Load test cases from a JSONL file (one JSON object per line).
std::vector<EquivalenceCase> load_equivalence_cases(const std::filesystem::path& jsonl);

// Result of running a single equivalence case.
struct EquivalenceResult {
    bool passed;
    std::string diff;  // what didn't match (empty on pass)
};

// Run a case: create AIAgent with FakeHttpTransport pre-loaded with
// model_responses, execute, compare output against expected.
EquivalenceResult run_equivalence(const EquivalenceCase& tc);

}  // namespace hermes::tests
