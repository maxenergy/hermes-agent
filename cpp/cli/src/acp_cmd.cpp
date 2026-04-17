// Implementation of the `hermes acp` stdio ACP server.  See
// `hermes/cli/acp_cmd.hpp` for the rationale.
//
// Design notes
// ------------
// * Logging: we write *nothing* to stdout outside of JSON-RPC responses.
//   All diagnostics go to std::cerr.  This mirrors Python's
//   `acp_adapter/entry.py::_setup_logging()`.
//
// * Transport: the task explicitly accepts newline-delimited JSON as a
//   pragmatic subset of the ACP transport.  The echo-test smoke invocation
//   from the task description uses exactly that form.  LSP-style
//   Content-Length framing can be layered in later without touching the
//   dispatch core.
//
// * PromptHandler: we instantiate a real `hermes::agent::AIAgent` lazily
//   on the first prompt using the same recipe as `HermesCLI::ensure_agent`
//   (OpenAI-compatible client when `OPENAI_API_KEY` is present, QwenClient
//   when `provider == "qwen"` is configured).  When no credentials are
//   available the handler returns `{status: "error", error:
//   "no_llm_provider", detail: ...}` — the client sees a deterministic
//   diagnostic instead of `method_not_available`.

#include "hermes/cli/acp_cmd.hpp"

#include "hermes/acp/acp_adapter.hpp"
#include "hermes/agent/ai_agent.hpp"
#include "hermes/agent/prompt_builder.hpp"
#include "hermes/auth/codex_oauth.hpp"
#include "hermes/auth/qwen_client.hpp"
#include "hermes/config/loader.hpp"
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/openai_client.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace hermes::cli {

namespace {

// Collect plain-text content from an ACP `params` payload.  The Python
// server accepts either `params.content` (a bare string) or a list of
// `TextContentBlock`s under `params.prompt.content`.  We accept both —
// whichever the client speaks — and fall back to an empty string.
std::string extract_prompt_text(const nlohmann::json& params) {
    if (params.contains("content") && params["content"].is_string()) {
        return params["content"].get<std::string>();
    }
    auto pull_blocks = [](const nlohmann::json& blocks) -> std::string {
        std::string out;
        if (!blocks.is_array()) return out;
        for (const auto& blk : blocks) {
            if (!blk.is_object()) continue;
            if (blk.contains("text") && blk["text"].is_string()) {
                if (!out.empty()) out.push_back('\n');
                out += blk["text"].get<std::string>();
            } else if (blk.contains("content") &&
                       blk["content"].is_string()) {
                if (!out.empty()) out.push_back('\n');
                out += blk["content"].get<std::string>();
            }
        }
        return out;
    };
    if (params.contains("prompt")) {
        const auto& prompt = params["prompt"];
        if (prompt.is_string()) return prompt.get<std::string>();
        if (prompt.is_object() && prompt.contains("content")) {
            auto t = pull_blocks(prompt["content"]);
            if (!t.empty()) return t;
        }
    }
    if (params.contains("blocks")) {
        auto t = pull_blocks(params["blocks"]);
        if (!t.empty()) return t;
    }
    return {};
}

// Tiny bridge that wraps an AIAgent + its collaborators so the
// PromptHandler can hold a stable reference for the lifetime of the
// ACP server process.
struct AgentBridge {
    nlohmann::json cfg;
    std::unique_ptr<hermes::llm::LlmClient> client;
    std::unique_ptr<hermes::agent::PromptBuilder> prompt_builder;
    std::unique_ptr<hermes::agent::AIAgent> agent;
    std::string construction_error;  // populated when we fail to build
};

std::unique_ptr<hermes::llm::LlmClient> make_llm_client(
    const nlohmann::json& cfg) {
    auto* transport = hermes::llm::get_default_transport();

    std::string provider;
    if (cfg.contains("provider") && cfg["provider"].is_string()) {
        provider = cfg["provider"].get<std::string>();
    }

    if (provider == "qwen") {
        return std::make_unique<hermes::auth::QwenClient>(transport);
    }

    if (provider == "openai-codex" || provider == "codex") {
        // Default to the local CLIProxyAPI which wraps the Codex
        // OAuth session; direct chatgpt.com access is blocked by
        // Cloudflare.  See hermes_cli.cpp::make_client for details.
        std::string codex_base = "http://127.0.0.1:8993/v1";
        if (cfg.contains("base_url") && cfg["base_url"].is_string() &&
            !cfg["base_url"].get<std::string>().empty()) {
            codex_base = cfg["base_url"].get<std::string>();
        }
        std::string token;
        if (auto* k = std::getenv("CLIPROXY_API_KEY")) token = k;
        if (token.empty()) {
            auto creds = hermes::auth::load_codex_credentials();
            if (creds) {
                if (!creds->access_token.empty()) token = creds->access_token;
                else if (!creds->api_key.empty()) token = creds->api_key;
            }
        }
        if (token.empty()) return nullptr;
        auto client = std::make_unique<hermes::llm::OpenAIClient>(
            transport, token, codex_base);
        client->set_provider_name("openai-codex");
        client->set_force_stream(true);
        return client;
    }

    std::string base_url = "https://api.openai.com/v1";
    if (cfg.contains("base_url") && cfg["base_url"].is_string() &&
        !cfg["base_url"].get<std::string>().empty()) {
        base_url = cfg["base_url"].get<std::string>();
    }

    std::string api_key;
    if (auto* k = std::getenv("OPENAI_API_KEY")) api_key = k;
    if (api_key.empty() && cfg.contains("provider_api_key") &&
        cfg["provider_api_key"].is_string()) {
        api_key = cfg["provider_api_key"].get<std::string>();
    }
    if (api_key.empty() && provider != "qwen") return nullptr;
    return std::make_unique<hermes::llm::OpenAIClient>(
        transport, api_key, base_url);
}

std::shared_ptr<AgentBridge> build_bridge() {
    auto br = std::make_shared<AgentBridge>();
    try {
        br->cfg = hermes::config::load_cli_config();
    } catch (...) {
        br->cfg = nlohmann::json::object();
    }

    br->client = make_llm_client(br->cfg);
    if (!br->client) {
        br->construction_error =
            "no LLM provider configured — set OPENAI_API_KEY or run "
            "`hermes login` before starting `hermes acp`";
        return br;
    }

    br->prompt_builder = std::make_unique<hermes::agent::PromptBuilder>();

    hermes::agent::AgentConfig acfg;
    if (br->cfg.contains("model") && br->cfg["model"].is_string()) {
        acfg.model = br->cfg["model"].get<std::string>();
    }
    if (br->cfg.contains("provider") && br->cfg["provider"].is_string()) {
        acfg.provider = br->cfg["provider"].get<std::string>();
    }
    acfg.platform = "acp";
    acfg.max_iterations = 30;

    // ToolDispatcher routes every tool call through the global registry.
    // The registry is populated lazily by the tool discovery pipeline;
    // for ACP we treat missing tools as a recoverable error and let the
    // agent surface them through the normal tool-result path.
    hermes::agent::ToolDispatcher dispatcher =
        [](const std::string& name, const nlohmann::json& args,
           const std::string& task_id) -> std::string {
            hermes::tools::ToolContext ctx;
            ctx.task_id = task_id;
            ctx.platform = "acp";
            try {
                return hermes::tools::ToolRegistry::instance().dispatch(
                    name, args, ctx);
            } catch (const std::exception& ex) {
                nlohmann::json err = {{"error", ex.what()},
                                      {"tool", name}};
                return err.dump();
            }
        };

    auto schemas =
        hermes::tools::ToolRegistry::instance().get_definitions();

    try {
        br->agent = std::make_unique<hermes::agent::AIAgent>(
            std::move(acfg), br->client.get(),
            /*session_db=*/nullptr,
            /*context_engine=*/nullptr,
            /*memory=*/nullptr,
            br->prompt_builder.get(),
            std::move(dispatcher),
            std::move(schemas));
    } catch (const std::exception& ex) {
        br->construction_error =
            std::string("AIAgent construction failed: ") + ex.what();
    }
    return br;
}

// Build the PromptHandler closure.  The bridge is cached inside the
// lambda so the AIAgent is only built once per ACP server process.
hermes::acp::AcpAdapter::PromptHandler make_prompt_handler() {
    auto state = std::make_shared<std::mutex>();
    auto bridge_slot = std::make_shared<std::shared_ptr<AgentBridge>>();

    return [state, bridge_slot](const nlohmann::json& params) -> nlohmann::json {
        std::shared_ptr<AgentBridge> bridge;
        {
            std::lock_guard<std::mutex> lk(*state);
            if (!*bridge_slot) {
                *bridge_slot = build_bridge();
            }
            bridge = *bridge_slot;
        }

        const std::string prompt_text = extract_prompt_text(params);
        if (prompt_text.empty()) {
            return nlohmann::json{
                {"error", "missing_prompt_content"},
                {"detail",
                 "params.content / params.prompt.content missing or empty"}};
        }

        if (!bridge || !bridge->agent) {
            const std::string detail =
                bridge ? bridge->construction_error
                       : std::string("agent bridge unavailable");
            return nlohmann::json{
                {"error", "no_llm_provider"},
                {"detail", detail},
                {"echo", prompt_text},
                {"role", "assistant"}};
        }

        try {
            auto result = bridge->agent->run_conversation(prompt_text);
            nlohmann::json out = {
                {"role", "assistant"},
                {"content", result.final_response},
                {"iterations", result.iterations_used},
                {"completed", result.completed},
            };
            if (result.error.has_value()) {
                out["error_detail"] = *result.error;
            }
            return out;
        } catch (const std::exception& ex) {
            return nlohmann::json{
                {"error", "agent_exception"},
                {"detail", ex.what()},
                {"echo", prompt_text}};
        }
    };
}

void print_help() {
    std::cerr <<
        "Usage: hermes acp [--help]\n"
        "\n"
        "Run the Hermes ACP (Agent Client Protocol) adapter over stdio.\n"
        "Intended to be launched by an editor host (Zed / VS Code / JetBrains).\n"
        "\n"
        "Transport: newline-delimited JSON-RPC on stdin/stdout.\n"
        "All logging is routed to stderr.\n"
        "\n"
        "Environment:\n"
        "  OPENAI_API_KEY / ANTHROPIC_API_KEY  — credentials used by the\n"
        "                                         adapter's default auth.\n"
        "  HERMES_HOME                          — profile root (default: ~/.hermes)\n";
}

bool is_help_flag(const char* s) {
    return std::strcmp(s, "--help") == 0 || std::strcmp(s, "-h") == 0;
}

}  // namespace

// Testable core: runs a single dispatch loop against the provided
// streams.  `adapter` is non-owning.  Exits when stdin closes or when
// a `shutdown` / `exit` method is received.
int run_acp_stdio_loop(hermes::acp::AcpAdapter& adapter,
                       std::istream& in,
                       std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        nlohmann::json req;
        try {
            req = nlohmann::json::parse(line);
        } catch (const std::exception& ex) {
            nlohmann::json err = {
                {"jsonrpc", "2.0"},
                {"error",
                 {{"code", -32700}, {"message", "parse_error"},
                  {"data", ex.what()}}}};
            out << err.dump() << "\n";
            out.flush();
            continue;
        }

        nlohmann::json resp = adapter.handle_request(req);

        // JSON-RPC envelope: echo id when present (null otherwise).
        nlohmann::json envelope = {{"jsonrpc", "2.0"}};
        if (req.contains("id")) envelope["id"] = req["id"];
        envelope["result"] = resp;

        out << envelope.dump() << "\n";
        out.flush();

        const auto method = req.value("method", std::string{});
        if (method == "shutdown" || method == "exit") break;
    }
    return 0;
}

int cmd_acp(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (is_help_flag(argv[i])) {
            print_help();
            return 0;
        }
    }

    std::cerr << "[hermes acp] starting stdio ACP adapter\n";

    hermes::acp::AcpConfig cfg;
    hermes::acp::AcpAdapter adapter(cfg);
    adapter.set_prompt_handler(make_prompt_handler());
    adapter.start();

    const int rc = run_acp_stdio_loop(adapter, std::cin, std::cout);

    adapter.stop();
    std::cerr << "[hermes acp] exiting (rc=" << rc << ")\n";
    return rc;
}

}  // namespace hermes::cli
