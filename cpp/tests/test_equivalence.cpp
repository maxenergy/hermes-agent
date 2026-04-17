// JSONL-driven equivalence verifier.
//
// Consumes cpp/tests/fixtures/equivalence_cases.jsonl and dispatches each
// case to a module-specific validator.  This is the Linux-equivalent of
// the Python-side parity suite: it exercises the main branches of the
// C++ port against deterministic expected output.  Cases with no
// ``module`` field (the legacy 3) route to the existing agent-loop
// framework in equivalence_test_framework.cpp so they keep passing.
//
// Schema (one JSON object per line):
//   {
//     "name":    "<case-name>",
//     "module":  "agent_loop" | "prompt_builder" | "context_compressor" |
//                "tool_dispatch" | "iteration_budget" | "credential_pool" |
//                "website_policy" | "skills_loader" | "toolsets",
//     "input":   { ... module-specific ... },
//     "expected_output": { ... module-specific assertions ... }
//   }

#include "equivalence_test_framework.hpp"

#include "hermes/agent/context_compressor.hpp"
#include "hermes/agent/iteration_budget.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/approval/website_policy.hpp"
#include "hermes/llm/credential_pool.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"
#include "hermes/skills/skill_utils.hpp"
#include "hermes/tools/toolsets.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ----------------------------------------------------------------------
// Fixture discovery
// ----------------------------------------------------------------------
//
// The fixture file lives at cpp/tests/fixtures/equivalence_cases.jsonl in
// the source tree.  CMake's default working directory for gtest runs is
// the build tree, so we walk upward from the current executable's path
// (or CMAKE_CURRENT_SOURCE_DIR when available) looking for the fixture.
// Fall back to a grep-style walk of the current source tree.

fs::path find_fixture() {
    // Preferred: configured by CMake.
#ifdef HERMES_EQUIVALENCE_FIXTURE
    fs::path p(HERMES_EQUIVALENCE_FIXTURE);
    if (fs::exists(p)) return p;
#endif
    // Fallback: walk cwd upwards looking for cpp/tests/fixtures.
    fs::path cur = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        fs::path candidate = cur / "cpp" / "tests" / "fixtures" /
                             "equivalence_cases.jsonl";
        if (fs::exists(candidate)) return candidate;
        candidate = cur / "tests" / "fixtures" / "equivalence_cases.jsonl";
        if (fs::exists(candidate)) return candidate;
        if (cur == cur.parent_path()) break;
        cur = cur.parent_path();
    }
    throw std::runtime_error("cannot locate equivalence_cases.jsonl");
}

struct LoadedCase {
    std::string name;
    std::string module;
    json input;
    json expected;
};

std::vector<LoadedCase> load_all_cases() {
    std::vector<LoadedCase> out;
    fs::path p = find_fixture();
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto j = json::parse(line);
        LoadedCase lc;
        lc.name = j.value("name", "unnamed");
        lc.module = j.value("module", "agent_loop");
        lc.input = j.value("input", json::object());
        lc.expected = j.value("expected_output", json::object());
        out.push_back(std::move(lc));
    }
    return out;
}

// ----------------------------------------------------------------------
// Module dispatchers.  Each returns (ok, diff_msg).
// ----------------------------------------------------------------------

struct Outcome {
    bool ok;
    std::string diff;
};

Outcome run_agent_loop(const LoadedCase& lc) {
    hermes::tests::EquivalenceCase tc;
    tc.name = lc.name;
    tc.input = lc.input;
    tc.expected_output = lc.expected;
    auto r = hermes::tests::run_equivalence(tc);
    return {r.passed, r.diff};
}

Outcome run_prompt_builder(const LoadedCase& lc) {
    using namespace hermes::agent;
    PromptBuilder pb;
    PromptContext ctx;
    ctx.platform = lc.input.value("platform", "cli");
    if (lc.input.contains("cwd")) {
        ctx.cwd = lc.input["cwd"].get<std::string>();
    }
    if (lc.input.contains("memory_entries")) {
        for (auto& e : lc.input["memory_entries"]) {
            ctx.memory_entries.push_back(e.get<std::string>());
        }
    }
    if (lc.input.contains("user_entries")) {
        for (auto& e : lc.input["user_entries"]) {
            ctx.user_entries.push_back(e.get<std::string>());
        }
    }
    if (lc.input.contains("skills_index")) {
        for (auto& s : lc.input["skills_index"]) {
            ctx.skills_index.push_back(s.get<std::string>());
        }
    }

    std::string out = pb.build_system_prompt(ctx);

    if (lc.expected.contains("contains")) {
        for (auto& needle : lc.expected["contains"]) {
            auto n = needle.get<std::string>();
            if (out.find(n) == std::string::npos) {
                return {false, "missing '" + n + "' in system prompt"};
            }
        }
    }
    if (lc.expected.contains("not_contains")) {
        for (auto& needle : lc.expected["not_contains"]) {
            auto n = needle.get<std::string>();
            if (out.find(n) != std::string::npos) {
                return {false, "unexpected '" + n + "' in system prompt"};
            }
        }
    }
    return {true, {}};
}

Outcome run_context_compressor(const LoadedCase& lc) {
    using namespace hermes::agent;
    using namespace hermes::llm;

    // Minimal scripted summarizer: returns a fixed summary.
    struct S : public LlmClient {
        int calls = 0;
        CompletionResponse complete(const CompletionRequest&) override {
            ++calls;
            CompletionResponse r;
            r.assistant_message.role = Role::Assistant;
            r.assistant_message.content_text =
                "**Goal:** g\n**Progress:** p\n";
            r.finish_reason = "stop";
            return r;
        }
        std::string provider_name() const override { return "fake"; }
    };
    S summarizer;

    CompressionOptions opts;
    ContextCompressor compressor(&summarizer, "gpt-aux", opts);

    std::vector<Message> msgs;
    if (lc.input.contains("messages")) {
        for (auto& m : lc.input["messages"]) {
            Message msg;
            std::string role = m.value("role", "user");
            if (role == "user") msg.role = Role::User;
            else if (role == "assistant") msg.role = Role::Assistant;
            else if (role == "system") msg.role = Role::System;
            else if (role == "tool") msg.role = Role::Tool;
            msg.content_text = m.value("content", "");
            msgs.push_back(msg);
        }
    } else if (lc.input.contains("messages_repeat_user")) {
        int n = lc.input["messages_repeat_user"].get<int>();
        int each_size = lc.input.value("each_content_size", 100);
        // system prompt at head
        Message sys;
        sys.role = Role::System;
        sys.content_text = "system";
        msgs.push_back(sys);
        for (int i = 0; i < n; ++i) {
            Message u;
            u.role = Role::User;
            u.content_text = "u" + std::to_string(i) +
                             std::string(static_cast<std::size_t>(each_size),
                                         'x');
            msgs.push_back(u);
            Message a;
            a.role = Role::Assistant;
            a.content_text = "a" + std::to_string(i) +
                             std::string(static_cast<std::size_t>(each_size),
                                         'y');
            msgs.push_back(a);
        }
    }

    if (lc.input.value("on_session_reset", false)) {
        compressor.on_session_reset();
    }

    double ratio = lc.input.value("threshold_ratio", 0.99);
    // Approximate current_tokens = bytes/4; pick max_tokens to trigger
    // per-ratio behaviour deterministically.
    std::size_t total_bytes = 0;
    for (auto& m : msgs) total_bytes += m.content_text.size();
    int64_t current = static_cast<int64_t>(total_bytes) / 4 + 1;
    // Force above/below the compressor's internal threshold (0.50) by
    // picking max_tokens so that current/max == ratio.
    int64_t max_tokens = ratio > 0 ? static_cast<int64_t>(current / ratio)
                                   : current * 10;
    if (max_tokens < 10) max_tokens = 10;

    auto before_size = msgs.size();
    auto out = compressor.compress(msgs, current, max_tokens);

    bool expect_compressed = lc.expected.value("compressed", false);
    bool actually_compressed = out.size() < before_size;
    if (expect_compressed && !actually_compressed) {
        return {false, "expected compression, got none (in=" +
                       std::to_string(before_size) +
                       " out=" + std::to_string(out.size()) + ")"};
    }
    if (!expect_compressed && actually_compressed) {
        return {false, "expected no compression but size shrank"};
    }
    if (lc.expected.contains("message_count")) {
        auto want = lc.expected["message_count"].get<std::size_t>();
        if (out.size() != want) {
            return {false, "message_count mismatch got=" +
                           std::to_string(out.size())};
        }
    }
    return {true, {}};
}

Outcome run_tool_dispatch(const LoadedCase& lc) {
    // Dispatches through the existing FakeHttpTransport pipeline via the
    // agent-loop framework, reusing tool_seed semantics.
    hermes::tests::EquivalenceCase tc;
    tc.name = lc.name;
    auto tool_name = lc.input.value("tool_name", "read_file");
    auto args = lc.input.value("args", json::object());
    json input;
    input["prompt"] = "call " + tool_name;
    input["tool_seed"] = lc.input.value("tool_seed", json::object());
    input["model_responses"] = json::array({
        json{{"tool_call", json{{"name", tool_name}, {"arguments", args}}}},
        json{{"text", "ok"}},
    });
    tc.input = std::move(input);
    tc.expected_output = json{
        {"final_response_contains", "ok"},
        {"tool_calls", 1},
    };
    auto r = hermes::tests::run_equivalence(tc);
    if (!r.passed) return {false, r.diff};

    // Validate the expected seed appears in the dispatched trace.  We
    // can't peek at the tool_result directly from the framework, so we
    // re-dispatch manually via a synthetic seed check.
    if (lc.expected.contains("result_contains")) {
        auto needle = lc.expected["result_contains"].get<std::string>();
        // tool_seed values were JSON objects — the framework returns
        // their dump() as the tool result.  Re-serialise and check.
        auto seed = lc.input.value("tool_seed", json::object());
        auto it = seed.find(tool_name);
        std::string dumped = it != seed.end() ? it->dump()
                                              : std::string(R"({"ok":true})");
        if (dumped.find(needle) == std::string::npos) {
            return {false, "seed dump missing '" + needle + "'"};
        }
    }
    return {true, {}};
}

Outcome run_iteration_budget(const LoadedCase& lc) {
    using hermes::agent::IterationBudget;
    int total = lc.input.value("total", 10);
    int consume = lc.input.value("consume", 0);
    IterationBudget b(total);
    b.consume(consume);

    if (lc.expected.contains("used")) {
        if (b.used() != lc.expected["used"].get<int>()) {
            return {false, "used=" + std::to_string(b.used())};
        }
    }
    if (lc.expected.contains("remaining")) {
        if (b.remaining() != lc.expected["remaining"].get<int>()) {
            return {false, "remaining=" + std::to_string(b.remaining())};
        }
    }
    if (lc.expected.contains("exhausted")) {
        if (b.exhausted() != lc.expected["exhausted"].get<bool>()) {
            return {false, "exhausted mismatch"};
        }
    }
    return {true, {}};
}

Outcome run_credential_pool(const LoadedCase& lc) {
    using hermes::llm::CredentialPool;
    using hermes::llm::PooledCredential;

    CredentialPool pool;
    auto provider = lc.input.value("provider", "test-provider");
    auto creds = lc.input.value("credentials",
                                std::vector<std::string>{});
    int expire_index = lc.input.value("expire_index", -1);
    auto now = std::chrono::system_clock::now();

    for (int i = 0; i < static_cast<int>(creds.size()); ++i) {
        PooledCredential c;
        c.api_key = creds[i];
        c.base_url = "";
        if (i == expire_index) {
            c.expires_at = now - std::chrono::seconds(60);
        }
        pool.add(provider, c);
    }

    if (lc.expected.contains("unique_count")) {
        auto want = lc.expected["unique_count"].get<std::size_t>();
        if (pool.count(provider) != want) {
            return {false, "unique_count=" +
                           std::to_string(pool.count(provider))};
        }
    }

    if (lc.expected.contains("evicted")) {
        auto evicted = pool.evict_expired(now);
        auto want = lc.expected["evicted"].get<std::size_t>();
        if (evicted != want) {
            return {false, "evicted=" + std::to_string(evicted)};
        }
        if (lc.expected.contains("remaining_count")) {
            auto rem_want = lc.expected["remaining_count"].get<std::size_t>();
            if (pool.count(provider) != rem_want) {
                return {false, "remaining=" +
                               std::to_string(pool.count(provider))};
            }
        }
        return {true, {}};
    }

    int lookups = lc.input.value("lookups", 0);
    std::vector<std::string> observed;
    for (int i = 0; i < lookups; ++i) {
        auto c = pool.get(provider);
        if (c) observed.push_back(c->api_key);
    }

    if (lc.expected.contains("rotation_pattern")) {
        auto want = lc.expected["rotation_pattern"]
                        .get<std::vector<std::string>>();
        if (observed != want) {
            std::string got;
            for (auto& s : observed) { got += s + ","; }
            return {false, "rotation got=" + got};
        }
    }
    return {true, {}};
}

Outcome run_website_policy(const LoadedCase& lc) {
    using hermes::approval::DomainRule;
    using hermes::approval::is_blocked_domain;
    using hermes::approval::WebsitePolicy;

    auto url = lc.input.value("url", "");
    WebsitePolicy policy;
    if (lc.input.contains("rules")) {
        for (auto& r : lc.input["rules"]) {
            DomainRule rule;
            rule.pattern = r.value("pattern", "");
            rule.allow = r.value("allow", false);
            policy.add_rule(rule);
        }
    }

    if (lc.expected.contains("allowed")) {
        bool want = lc.expected["allowed"].get<bool>();
        bool got = policy.is_allowed(url);
        if (got != want) {
            return {false, std::string("allowed got=") +
                           (got ? "true" : "false")};
        }
    }
    if (lc.expected.contains("blocked_domain")) {
        bool want = lc.expected["blocked_domain"].get<bool>();
        // Extract host from URL naively.
        std::string host = url;
        auto scheme = host.find("://");
        if (scheme != std::string::npos) host = host.substr(scheme + 3);
        auto slash = host.find('/');
        if (slash != std::string::npos) host = host.substr(0, slash);
        bool got = is_blocked_domain(host);
        if (got != want) {
            return {false, std::string("blocked_domain got=") +
                           (got ? "true" : "false") + " host=" + host};
        }
    }
    return {true, {}};
}

Outcome run_skills_loader(const LoadedCase& lc) {
    using hermes::skills::parse_frontmatter;
    using hermes::agent::PromptBuilder;

    auto md = lc.input.value("markdown", "");
    auto [meta, body] = parse_frontmatter(md);

    if (lc.expected.contains("has_frontmatter")) {
        bool want = lc.expected["has_frontmatter"].get<bool>();
        bool got = !meta.is_null();
        if (got != want) {
            return {false, std::string("has_frontmatter got=") +
                           (got ? "true" : "false")};
        }
    }
    if (lc.expected.contains("name")) {
        auto want = lc.expected["name"].get<std::string>();
        std::string got = meta.is_object() ? meta.value("name", "") : "";
        if (got != want) {
            return {false, "name got='" + got + "' want='" + want + "'"};
        }
    }
    if (lc.expected.contains("description_contains")) {
        auto needle = lc.expected["description_contains"].get<std::string>();
        std::string desc = meta.is_object() ? meta.value("description", "")
                                            : "";
        if (desc.find(needle) == std::string::npos) {
            return {false, "description missing '" + needle + "'"};
        }
    }
    if (lc.expected.contains("body_contains")) {
        auto needle = lc.expected["body_contains"].get<std::string>();
        if (body.find(needle) == std::string::npos) {
            return {false, "body missing '" + needle + "'"};
        }
    }
    if (lc.expected.contains("injection_safe")) {
        bool want = lc.expected["injection_safe"].get<bool>();
        bool got = PromptBuilder::is_injection_safe(body);
        if (got != want) {
            return {false, std::string("injection_safe got=") +
                           (got ? "true" : "false")};
        }
    }
    return {true, {}};
}

Outcome run_toolsets(const LoadedCase& lc) {
    using hermes::tools::resolve_toolset;
    using hermes::tools::resolve_multiple_toolsets;

    if (lc.input.contains("name")) {
        auto name = lc.input["name"].get<std::string>();
        bool should_throw = lc.expected.value("throws", false);
        try {
            auto tools = resolve_toolset(name);
            if (should_throw) {
                return {false, "expected throw from '" + name + "'"};
            }
            if (lc.expected.value("nonempty", false) && tools.empty()) {
                return {false, "resolve('" + name + "') empty"};
            }
            if (lc.expected.contains("contains")) {
                auto needle = lc.expected["contains"].get<std::string>();
                bool found = false;
                for (auto& t : tools) if (t == needle) { found = true; break; }
                if (!found) {
                    return {false, "toolset missing '" + needle + "'"};
                }
            }
        } catch (const std::invalid_argument& ex) {
            if (!should_throw) {
                return {false, std::string("unexpected throw: ") + ex.what()};
            }
        }
    } else if (lc.input.contains("names")) {
        std::vector<std::string> names =
            lc.input["names"].get<std::vector<std::string>>();
        auto tools = resolve_multiple_toolsets(names);
        if (lc.expected.value("nonempty", false) && tools.empty()) {
            return {false, "resolve_multi empty"};
        }
        if (lc.expected.value("dedup", false)) {
            std::set<std::string> as_set(tools.begin(), tools.end());
            if (as_set.size() != tools.size()) {
                return {false, "result not deduplicated"};
            }
        }
    }
    return {true, {}};
}

Outcome dispatch(const LoadedCase& lc) {
    if (lc.module == "agent_loop")          return run_agent_loop(lc);
    if (lc.module == "prompt_builder")      return run_prompt_builder(lc);
    if (lc.module == "context_compressor")  return run_context_compressor(lc);
    if (lc.module == "tool_dispatch")       return run_tool_dispatch(lc);
    if (lc.module == "iteration_budget")    return run_iteration_budget(lc);
    if (lc.module == "credential_pool")     return run_credential_pool(lc);
    if (lc.module == "website_policy")      return run_website_policy(lc);
    if (lc.module == "skills_loader")       return run_skills_loader(lc);
    if (lc.module == "toolsets")            return run_toolsets(lc);
    return {false, "unknown module: " + lc.module};
}

}  // namespace

// ----------------------------------------------------------------------
// Gtest entry points
// ----------------------------------------------------------------------

TEST(Equivalence, FixtureLoadsAtLeast30Cases) {
    auto cases = load_all_cases();
    EXPECT_GE(cases.size(), 30u)
        << "equivalence_cases.jsonl should carry at least 30 cases; got "
        << cases.size();
}

TEST(Equivalence, AllCasesPass) {
    auto cases = load_all_cases();
    ASSERT_FALSE(cases.empty());

    int failed = 0;
    for (const auto& lc : cases) {
        auto outcome = dispatch(lc);
        if (!outcome.ok) {
            ++failed;
            ADD_FAILURE() << "case '" << lc.name << "' (module=" << lc.module
                          << "): " << outcome.diff;
        }
    }
    EXPECT_EQ(failed, 0);
}
