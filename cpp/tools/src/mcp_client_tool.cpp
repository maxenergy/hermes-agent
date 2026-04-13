#include "hermes/tools/mcp_client_tool.hpp"

#include "hermes/tools/mcp_transport.hpp"
#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

// Optional LLM client integration for sampling.
#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"

namespace hermes::tools {

using json = nlohmann::json;

void McpClientManager::load_config(const json& mcp_servers_json) {
    configs_.clear();
    if (!mcp_servers_json.is_object()) return;

    for (auto& [name, val] : mcp_servers_json.items()) {
        McpServerConfig cfg;
        cfg.name = name;
        cfg.command = val.value("command", "");

        if (val.contains("args") && val["args"].is_array()) {
            for (const auto& a : val["args"]) {
                if (a.is_string()) cfg.args.push_back(a.get<std::string>());
            }
        }

        cfg.url = val.value("url", "");
        cfg.timeout = val.value("timeout", 120);
        cfg.connect_timeout = val.value("connect_timeout", 60);

        if (val.contains("headers") && val["headers"].is_object()) {
            for (auto& [k, v] : val["headers"].items()) {
                if (v.is_string()) cfg.headers[k] = v.get<std::string>();
            }
        }

        if (val.contains("env") && val["env"].is_object()) {
            for (auto& [k, v] : val["env"].items()) {
                if (v.is_string()) cfg.env[k] = v.get<std::string>();
            }
        }

        if (val.contains("sampling") && val["sampling"].is_object()) {
            auto& s = val["sampling"];
            cfg.sampling.enabled = s.value("enabled", false);
            cfg.sampling.model = s.value("model", "");
            cfg.sampling.max_tokens_cap = s.value("max_tokens_cap", 4096);
            cfg.sampling.timeout = s.value("timeout", 30);
            cfg.sampling.max_rpm = s.value("max_rpm", 10);
        }

        if (val.contains("reconnect") && val["reconnect"].is_object()) {
            auto& r = val["reconnect"];
            cfg.reconnect_enabled = r.value("enabled", true);
            cfg.reconnect_initial_ms = r.value("initial_ms", 500);
            cfg.reconnect_max_ms = r.value("max_ms", 30000);
            cfg.reconnect_max_attempts = r.value("max_attempts", 5);
        } else if (val.contains("reconnect") && val["reconnect"].is_boolean()) {
            cfg.reconnect_enabled = val["reconnect"].get<bool>();
        }

        configs_[name] = std::move(cfg);
    }
}

std::vector<std::string> McpClientManager::server_names() const {
    std::vector<std::string> names;
    names.reserve(configs_.size());
    for (const auto& [name, _] : configs_) {
        names.push_back(name);
    }
    return names;
}

std::optional<McpServerConfig> McpClientManager::get_config(
    const std::string& name) const {
    auto it = configs_.find(name);
    if (it == configs_.end()) return std::nullopt;
    return it->second;
}

bool McpClientManager::connect(const std::string& server_name) {
    auto it = configs_.find(server_name);
    if (it == configs_.end()) return false;

    auto cfg = it->second;
    if (cfg.command.empty()) return false;

    // Merge any cached OAuth token into the env.  Some servers read
    // MCP_ACCESS_TOKEN; we pass it through as an opt-in channel.
    auto env = cfg.env;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto tok_it = oauth_tokens_.find(server_name);
        if (tok_it != oauth_tokens_.end() && !tok_it->second.empty()) {
            env["MCP_ACCESS_TOKEN"] = tok_it->second;
        }
    }

    try {
        auto transport = std::make_shared<McpStdioTransport>(
            cfg.command, cfg.args, env);
        transport->initialize();
        transports_[server_name] = transport;
        return true;
    } catch (const std::exception& ex) {
        // Detect a 401-ish failure to trigger OAuth flow.
        std::string msg = ex.what();
        bool looks_like_auth =
            msg.find("401") != std::string::npos ||
            msg.find("unauthorized") != std::string::npos ||
            msg.find("WWW-Authenticate") != std::string::npos;
        if (looks_like_auth && oauth_initiator_) {
            std::string www_auth;
            auto pos = msg.find("WWW-Authenticate");
            if (pos != std::string::npos) www_auth = msg.substr(pos);
            std::string tok = oauth_initiator_(server_name, www_auth);
            if (!tok.empty()) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    oauth_tokens_[server_name] = tok;
                }
                // One retry with fresh token.
                auto env2 = cfg.env;
                env2["MCP_ACCESS_TOKEN"] = tok;
                try {
                    auto t2 = std::make_shared<McpStdioTransport>(
                        cfg.command, cfg.args, env2);
                    t2->initialize();
                    transports_[server_name] = t2;
                    return true;
                } catch (...) {
                    return false;
                }
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool McpClientManager::connect_with_backoff_(const std::string& server_name) {
    auto it = configs_.find(server_name);
    if (it == configs_.end()) return false;
    const auto& cfg = it->second;

    if (!cfg.reconnect_enabled) return connect(server_name);

    int delay_ms = cfg.reconnect_initial_ms;
    int attempts = cfg.reconnect_max_attempts > 0 ? cfg.reconnect_max_attempts : 1;

    // Jitter RNG (thread-local for determinism in tests).
    static thread_local std::mt19937 rng{std::random_device{}()};

    for (int i = 0; i < attempts; ++i) {
        if (connect(server_name)) return true;
        if (i + 1 >= attempts) break;
        // ±20% jitter → [0.8, 1.2] × delay_ms
        std::uniform_real_distribution<double> jitter(0.8, 1.2);
        int d = static_cast<int>(delay_ms * jitter(rng));
        std::this_thread::sleep_for(std::chrono::milliseconds(d));
        delay_ms = std::min(delay_ms * 2, cfg.reconnect_max_ms);
    }
    return false;
}

void McpClientManager::disconnect(const std::string& server_name) {
    auto it = transports_.find(server_name);
    if (it != transports_.end()) {
        it->second->shutdown();
        transports_.erase(it);
    }
}

bool McpClientManager::is_connected(const std::string& server_name) const {
    auto it = transports_.find(server_name);
    if (it == transports_.end()) return false;
    return it->second->is_connected();
}

void McpClientManager::register_server_tools(const std::string& server_name,
                                              ToolRegistry& registry) {
    auto cfg_it = configs_.find(server_name);
    if (cfg_it == configs_.end()) return;

    const auto& cfg = cfg_it->second;
    server_registry_[server_name] = &registry;

    // If the server has a command, try to connect and discover tools.
    if (!cfg.command.empty()) {
        // Connect if not already connected (with backoff).
        if (transports_.find(server_name) == transports_.end()) {
            if (!connect_with_backoff_(server_name)) {
                goto register_fallback;
            }
        }

        {
            auto transport = transports_[server_name];
            install_inbound_handler_(server_name, &registry);
            std::vector<json> tools;
            try {
                tools = transport->list_tools();
            } catch (...) {
                goto register_fallback;
            }

            std::set<std::string> registered;
            for (const auto& tool_def : tools) {
                ToolEntry e;
                std::string tool_name = tool_def.value("name", "");
                if (tool_name.empty()) continue;

                e.name = "mcp_" + server_name + "_" + tool_name;
                e.toolset = "mcp";
                e.description = tool_def.value("description",
                    "MCP tool: " + tool_name + " (server: " + server_name + ")");
                e.emoji = "\xf0\x9f\x94\x8c";  // plug

                if (tool_def.contains("inputSchema")) {
                    e.schema = tool_def["inputSchema"];
                } else {
                    e.schema = json::parse(R"JSON({
                        "type": "object",
                        "properties": {},
                        "additionalProperties": true
                    })JSON");
                }

                auto captured_server = server_name;
                auto captured_tool_name = tool_name;
                auto timeout_secs = cfg.timeout;
                auto* self = this;

                e.handler = [self, captured_server, captured_tool_name,
                             timeout_secs](const json& args,
                                           const ToolContext& /*ctx*/) -> std::string {
                    // Reconnect-on-failure: one retry through backoff.
                    for (int attempt = 0; attempt < 2; ++attempt) {
                        auto t_it = self->transports_.find(captured_server);
                        std::shared_ptr<McpStdioTransport> transport;
                        if (t_it != self->transports_.end()) transport = t_it->second;

                        if (!transport || !transport->is_connected()) {
                            self->transports_.erase(captured_server);
                            if (!self->connect_with_backoff_(captured_server)) {
                                return tool_error(
                                    "MCP server disconnected and reconnect failed: " +
                                    captured_server);
                            }
                            transport = self->transports_[captured_server];
                            self->install_inbound_handler_(
                                captured_server,
                                self->server_registry_[captured_server]);
                        }
                        try {
                            auto result = transport->call_tool(
                                captured_tool_name, args);
                            return tool_result(result);
                        } catch (const std::exception& ex) {
                            if (attempt == 0 && !transport->is_connected()) {
                                self->transports_.erase(captured_server);
                                continue;  // retry once after reconnect
                            }
                            return tool_error(
                                std::string("MCP call failed: ") + ex.what());
                        }
                    }
                    return tool_error("MCP call failed after retry");
                };

                registered.insert(e.name);
                registry.register_tool(std::move(e));
            }
            server_tool_names_[server_name] = std::move(registered);
            return;  // Success — registered real tools.
        }
    }

register_fallback:
    // Fallback: register a proxy tool that reports the server is not connected.
    ToolEntry e;
    e.name = "mcp_" + server_name;
    e.toolset = "mcp";
    e.description = "Call MCP server: " + server_name;
    e.emoji = "\xf0\x9f\x94\x8c";  // plug

    e.schema = json::parse(R"JSON({
        "type": "object",
        "properties": {
            "tool": {
                "type": "string",
                "description": "Tool name to call on the MCP server"
            },
            "arguments": {
                "type": "object",
                "description": "Arguments to pass to the tool"
            }
        },
        "required": ["tool"]
    })JSON");

    e.handler = [server_name](const json& /*args*/,
                              const ToolContext& /*ctx*/) -> std::string {
        return tool_error("MCP server not connected: " + server_name);
    };

    server_tool_names_[server_name].insert(e.name);
    registry.register_tool(std::move(e));
}

// -- Sampling / discovery / OAuth ----------------------------------------

void McpClientManager::set_llm_client(hermes::llm::LlmClient* client) {
    std::lock_guard<std::mutex> lk(mu_);
    llm_client_ = client;
}

void McpClientManager::set_allow_sampling(bool allow) {
    std::lock_guard<std::mutex> lk(mu_);
    allow_sampling_ = allow;
}

bool McpClientManager::allow_sampling() const {
    std::lock_guard<std::mutex> lk(mu_);
    return allow_sampling_;
}

void McpClientManager::set_sampling_approver(SamplingApprover approver) {
    std::lock_guard<std::mutex> lk(mu_);
    sampling_approver_ = std::move(approver);
}

void McpClientManager::set_oauth_initiator(OAuthInitiator fn) {
    std::lock_guard<std::mutex> lk(mu_);
    oauth_initiator_ = std::move(fn);
}

std::string McpClientManager::run_oauth_flow(const std::string& server_name,
                                             const std::string& www_authenticate) {
    OAuthInitiator fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn = oauth_initiator_;
    }
    if (!fn) return {};
    std::string tok = fn(server_name, www_authenticate);
    if (!tok.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        oauth_tokens_[server_name] = tok;
    }
    return tok;
}

void McpClientManager::inject_transport_for_testing(
    const std::string& server_name,
    std::shared_ptr<McpStdioTransport> transport) {
    transports_[server_name] = std::move(transport);
}

void McpClientManager::install_inbound_handler_(const std::string& server_name,
                                                ToolRegistry* registry) {
    auto it = transports_.find(server_name);
    if (it == transports_.end() || !it->second) return;
    auto* self = this;
    auto reg_ptr = registry;
    auto sn = server_name;
    it->second->set_inbound_handler(
        [self, sn, reg_ptr](const std::string& method,
                            const json& params) -> json {
            // notifications/tools/list_changed → refresh
            if (method == "notifications/tools/list_changed" ||
                method == "tools/list_changed") {
                if (reg_ptr) self->refresh_server_tools(sn, *reg_ptr);
                return json::object();
            }
            return self->handle_inbound_(sn, method, params);
        });
}

void McpClientManager::refresh_server_tools(const std::string& server_name,
                                            ToolRegistry& registry) {
    auto cfg_it = configs_.find(server_name);
    if (cfg_it == configs_.end()) return;
    auto t_it = transports_.find(server_name);
    if (t_it == transports_.end() || !t_it->second) return;

    // Remove previously-registered tool entries for this server.
    auto tnames = server_tool_names_[server_name];
    for (const auto& name : tnames) {
        registry.deregister(name);
    }
    server_tool_names_[server_name].clear();

    std::vector<json> tools;
    try {
        tools = t_it->second->list_tools();
    } catch (...) {
        return;
    }

    auto transport = t_it->second;
    const auto& cfg = cfg_it->second;
    std::set<std::string> registered;
    for (const auto& tool_def : tools) {
        ToolEntry e;
        std::string tool_name = tool_def.value("name", "");
        if (tool_name.empty()) continue;
        e.name = "mcp_" + server_name + "_" + tool_name;
        e.toolset = "mcp";
        e.description = tool_def.value("description",
            "MCP tool: " + tool_name + " (server: " + server_name + ")");
        e.emoji = "\xf0\x9f\x94\x8c";
        if (tool_def.contains("inputSchema")) {
            e.schema = tool_def["inputSchema"];
        } else {
            e.schema = json::parse(R"({"type":"object","properties":{},"additionalProperties":true})");
        }
        auto captured_server = server_name;
        auto captured_tool = tool_name;
        auto timeout_secs = cfg.timeout;
        auto* self = this;
        e.handler = [self, captured_server, captured_tool, timeout_secs](
                        const json& args, const ToolContext&) -> std::string {
            auto t_it = self->transports_.find(captured_server);
            if (t_it == self->transports_.end() || !t_it->second) {
                return tool_error("MCP server not connected: " + captured_server);
            }
            try {
                auto r = t_it->second->call_tool(captured_tool, args);
                return tool_result(r);
            } catch (const std::exception& ex) {
                return tool_error(std::string("MCP call failed: ") + ex.what());
            }
        };
        registered.insert(e.name);
        registry.register_tool(std::move(e));
    }
    server_tool_names_[server_name] = std::move(registered);
}

json McpClientManager::handle_inbound_(const std::string& server_name,
                                       const std::string& method,
                                       const json& params) {
    // MCP server-to-client: sampling/createMessage.
    if (method == "sampling/createMessage") {
        hermes::llm::LlmClient* client = nullptr;
        bool allow = false;
        SamplingApprover approver;
        {
            std::lock_guard<std::mutex> lk(mu_);
            client = llm_client_;
            allow = allow_sampling_;
            approver = sampling_approver_;
        }
        if (!allow) {
            throw std::runtime_error(
                "sampling disabled by mcp.allow_sampling=false for " + server_name);
        }
        if (approver && !approver(server_name, params)) {
            throw std::runtime_error("sampling denied by approver for " +
                                     server_name);
        }
        if (!client) {
            throw std::runtime_error("no LLM client configured for sampling");
        }

        auto cfg_it = configs_.find(server_name);
        const auto* scfg = cfg_it != configs_.end() ? &cfg_it->second : nullptr;

        // Translate MCP sampling params → CompletionRequest.
        hermes::llm::CompletionRequest req;
        req.model = scfg && !scfg->sampling.model.empty()
                        ? scfg->sampling.model
                        : params.value("modelPreferences", json::object())
                              .value("hints", json::array())
                              .empty()
                            ? std::string()
                            : params["modelPreferences"]["hints"][0]
                                  .value("name", "");
        int cap = scfg ? scfg->sampling.max_tokens_cap : 4096;
        int requested = params.value("maxTokens", cap);
        req.max_tokens = std::min(requested, cap);
        if (params.contains("temperature") && params["temperature"].is_number()) {
            req.temperature = params["temperature"].get<double>();
        }

        if (params.contains("systemPrompt") &&
            params["systemPrompt"].is_string()) {
            hermes::llm::Message sys;
            sys.role = hermes::llm::Role::System;
            sys.content_text = params["systemPrompt"].get<std::string>();
            req.messages.push_back(std::move(sys));
        }
        if (params.contains("messages") && params["messages"].is_array()) {
            for (const auto& m : params["messages"]) {
                hermes::llm::Message msg;
                std::string role = m.value("role", "user");
                msg.role = (role == "assistant") ? hermes::llm::Role::Assistant
                                                 : hermes::llm::Role::User;
                if (m.contains("content")) {
                    const auto& c = m["content"];
                    if (c.is_string()) {
                        msg.content_text = c.get<std::string>();
                    } else if (c.is_object()) {
                        std::string t = c.value("type", "");
                        if (t == "text") {
                            msg.content_text = c.value("text", "");
                        } else {
                            msg.content_text = c.dump();
                        }
                    }
                }
                req.messages.push_back(std::move(msg));
            }
        }

        auto resp = client->complete(req);

        // Build MCP sampling result.
        json out;
        out["role"] = "assistant";
        out["model"] = resp.raw.value("model", req.model);
        out["stopReason"] = resp.finish_reason.empty() ? "endTurn"
                                                       : resp.finish_reason;
        json content;
        content["type"] = "text";
        content["text"] = resp.assistant_message.content_text;
        out["content"] = content;
        return out;
    }

    // notifications/tools/list_changed handled in install_inbound_handler_
    // (this path is reached only when registry pointer is missing).
    if (method == "notifications/tools/list_changed" ||
        method == "tools/list_changed") {
        auto reg_it = server_registry_.find(server_name);
        if (reg_it != server_registry_.end() && reg_it->second) {
            refresh_server_tools(server_name, *reg_it->second);
        }
        return json::object();
    }

    // Unknown server-originated method → JSON-RPC method-not-found.
    throw std::runtime_error("method not supported: " + method);
}

}  // namespace hermes::tools
