#include "hermes/tools/rl_training_tool.hpp"
#include "hermes/tools/registry.hpp"

#include <cstdlib>
#include <string>
#include <unordered_map>

namespace hermes::tools {

namespace {

bool rl_env_available() {
    return std::getenv("NOUS_RL_API_URL") != nullptr &&
           std::getenv("NOUS_RL_API_KEY") != nullptr;
}

std::string api_url() {
    const char* v = std::getenv("NOUS_RL_API_URL");
    return v ? std::string(v) : std::string();
}

std::string api_key() {
    const char* v = std::getenv("NOUS_RL_API_KEY");
    return v ? std::string(v) : std::string();
}

std::unordered_map<std::string, std::string> auth_headers() {
    return {{"Authorization", "Bearer " + api_key()},
            {"Content-Type", "application/json"}};
}

// Shared helper: issue a GET via POST with empty body (HttpTransport only
// exposes post_json).  For GET semantics we post an empty body and rely on
// the API ignoring it.
std::string http_get(hermes::llm::HttpTransport* tp, const std::string& url) {
    auto resp = tp->post_json(url, auth_headers(), "");
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return tool_error("RL API error (HTTP " +
                          std::to_string(resp.status_code) + "): " + resp.body);
    }
    return tool_result(nlohmann::json::parse(resp.body));
}

std::string http_post(hermes::llm::HttpTransport* tp,
                      const std::string& url,
                      const nlohmann::json& body) {
    auto resp = tp->post_json(url, auth_headers(), body.dump());
    if (resp.status_code < 200 || resp.status_code >= 300) {
        return tool_error("RL API error (HTTP " +
                          std::to_string(resp.status_code) + "): " + resp.body);
    }
    return tool_result(nlohmann::json::parse(resp.body));
}

std::string http_patch(hermes::llm::HttpTransport* tp,
                       const std::string& url,
                       const nlohmann::json& body) {
    // HttpTransport only has post_json — we use it for PATCH as well;
    // the RL API distinguishes by URL pattern.
    return http_post(tp, url, body);
}

}  // namespace

void register_rl_tools(hermes::llm::HttpTransport* transport) {
    auto& reg = ToolRegistry::instance();
    reg.register_toolset_check("rl", rl_env_available);

    auto* tp = transport;

    // 1. rl_list_environments
    {
        ToolEntry e;
        e.name = "rl_list_environments";
        e.toolset = "rl";
        e.description = "List available RL training environments";
        e.emoji = "\xF0\x9F\x8C\x8D";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = [tp](const nlohmann::json& /*args*/,
                         const ToolContext& /*ctx*/) -> std::string {
            return http_get(tp, api_url() + "/environments");
        };
        reg.register_tool(std::move(e));
    }

    // 2. rl_select_environment
    {
        ToolEntry e;
        e.name = "rl_select_environment";
        e.toolset = "rl";
        e.description = "Select an RL training environment";
        e.emoji = "\xF0\x9F\x8E\xAF";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "Environment name"}}}}},
            {"required", nlohmann::json::array({"name"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            nlohmann::json body;
            body["name"] = args.at("name");
            return http_post(tp, api_url() + "/environments/select", body);
        };
        reg.register_tool(std::move(e));
    }

    // 3. rl_get_current_config
    {
        ToolEntry e;
        e.name = "rl_get_current_config";
        e.toolset = "rl";
        e.description = "Get current RL training configuration";
        e.emoji = "\xE2\x9A\x99";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = [tp](const nlohmann::json& /*args*/,
                         const ToolContext& /*ctx*/) -> std::string {
            return http_get(tp, api_url() + "/config");
        };
        reg.register_tool(std::move(e));
    }

    // 4. rl_edit_config
    {
        ToolEntry e;
        e.name = "rl_edit_config";
        e.toolset = "rl";
        e.description = "Edit RL training configuration";
        e.emoji = "\xE2\x9C\x8F";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"key", {{"type", "string"}, {"description", "Config key"}}},
              {"value", {{"description", "New value"}}}}},
            {"required", nlohmann::json::array({"key", "value"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            nlohmann::json body;
            body["key"] = args.at("key");
            body["value"] = args.at("value");
            return http_patch(tp, api_url() + "/config", body);
        };
        reg.register_tool(std::move(e));
    }

    // 5. rl_start_training
    {
        ToolEntry e;
        e.name = "rl_start_training";
        e.toolset = "rl";
        e.description = "Start an RL training run";
        e.emoji = "\xF0\x9F\x9A\x80";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = [tp](const nlohmann::json& /*args*/,
                         const ToolContext& /*ctx*/) -> std::string {
            return http_post(tp, api_url() + "/training/start",
                             nlohmann::json::object());
        };
        reg.register_tool(std::move(e));
    }

    // 6. rl_check_status
    {
        ToolEntry e;
        e.name = "rl_check_status";
        e.toolset = "rl";
        e.description = "Check status of an RL training run";
        e.emoji = "\xF0\x9F\x93\x8A";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id", {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            auto run_id = args.at("run_id").get<std::string>();
            return http_get(tp, api_url() + "/training/" + run_id);
        };
        reg.register_tool(std::move(e));
    }

    // 7. rl_stop_training
    {
        ToolEntry e;
        e.name = "rl_stop_training";
        e.toolset = "rl";
        e.description = "Stop an RL training run";
        e.emoji = "\xE2\x9B\x94";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id", {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            auto run_id = args.at("run_id").get<std::string>();
            return http_post(tp, api_url() + "/training/" + run_id + "/stop",
                             nlohmann::json::object());
        };
        reg.register_tool(std::move(e));
    }

    // 8. rl_get_results
    {
        ToolEntry e;
        e.name = "rl_get_results";
        e.toolset = "rl";
        e.description = "Get results of an RL training run";
        e.emoji = "\xF0\x9F\x93\x88";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id", {{"type", "string"}, {"description", "Training run ID"}}}}},
            {"required", nlohmann::json::array({"run_id"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            auto run_id = args.at("run_id").get<std::string>();
            return http_get(tp, api_url() + "/training/" + run_id + "/results");
        };
        reg.register_tool(std::move(e));
    }

    // 9. rl_list_runs
    {
        ToolEntry e;
        e.name = "rl_list_runs";
        e.toolset = "rl";
        e.description = "List all RL training runs";
        e.emoji = "\xF0\x9F\x93\x8B";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = [tp](const nlohmann::json& /*args*/,
                         const ToolContext& /*ctx*/) -> std::string {
            return http_get(tp, api_url() + "/training/runs");
        };
        reg.register_tool(std::move(e));
    }

    // 10b. score_trajectory — reward-model score for a finished
    // conversation, used offline to filter SFT data / compute RLHF
    // rewards.  POSTs the trajectory to ``/score`` on the RL API.
    {
        ToolEntry e;
        e.name = "score_trajectory";
        e.toolset = "rl";
        e.description =
            "Score a completed trajectory (conversations list) with the "
            "configured RL reward model; returns {score, breakdown}.";
        e.emoji = "\xF0\x9F\x8F\x85";  // medal
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"conversations",
               {{"type", "array"},
                {"description",
                 "HuggingFace SFT conversations array (from/value pairs)"}}},
              {"reward_model",
               {{"type", "string"},
                {"description", "Optional reward model id"}}}}},
            {"required", nlohmann::json::array({"conversations"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            nlohmann::json body;
            body["conversations"] = args.at("conversations");
            if (args.contains("reward_model")) {
                body["reward_model"] = args["reward_model"];
            }
            return http_post(tp, api_url() + "/score", body);
        };
        reg.register_tool(std::move(e));
    }

    // 10. rl_test_inference (bonus)
    {
        ToolEntry e;
        e.name = "rl_test_inference";
        e.toolset = "rl";
        e.description = "Test inference with a trained RL model";
        e.emoji = "\xF0\x9F\xA7\xAA";
        e.check_fn = rl_env_available;
        e.requires_env = {"NOUS_RL_API_URL", "NOUS_RL_API_KEY"};
        e.schema = {
            {"type", "object"},
            {"properties",
             {{"run_id", {{"type", "string"}, {"description", "Training run ID"}}},
              {"input", {{"type", "string"}, {"description", "Input for inference"}}}}},
            {"required", nlohmann::json::array({"run_id", "input"})}};
        e.handler = [tp](const nlohmann::json& args,
                         const ToolContext& /*ctx*/) -> std::string {
            auto run_id = args.at("run_id").get<std::string>();
            nlohmann::json body;
            body["input"] = args.at("input");
            return http_post(tp, api_url() + "/training/" + run_id + "/infer",
                             body);
        };
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
