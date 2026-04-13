#include "hermes/batch/hf_schema.hpp"

#include <sstream>

namespace hermes::batch {

namespace {

// Parse the arguments field of a tool_call.  The OpenAI spec stores it
// as a JSON string; we accept either a string (parsed once) or an
// already-parsed object.
nlohmann::json coerce_arguments(const nlohmann::json& raw) {
    if (raw.is_string()) {
        try {
            return nlohmann::json::parse(raw.get<std::string>());
        } catch (...) {
            return nlohmann::json::object();
        }
    }
    if (raw.is_null()) {
        return nlohmann::json::object();
    }
    return raw;
}

// Ensure every gpt turn begins with a <think> block so the trained
// format is consistent — mirrors the Python logic.
std::string ensure_think_block(const std::string& body) {
    if (body.find("<think>") == std::string::npos) {
        return "<think>\n</think>\n" + body;
    }
    return body;
}

}  // namespace

std::string format_tool_call(const std::string& name,
                              const nlohmann::json& arguments) {
    nlohmann::json wrapper;
    wrapper["name"] = name;
    wrapper["arguments"] = arguments.is_null() ? nlohmann::json::object()
                                                : arguments;
    std::ostringstream os;
    os << "<tool_call>\n" << wrapper.dump() << "\n</tool_call>\n";
    return os.str();
}

std::string format_tool_response(const std::string& tool_call_id,
                                  const std::string& name,
                                  const nlohmann::json& content) {
    nlohmann::json wrapper;
    wrapper["tool_call_id"] = tool_call_id;
    wrapper["name"] = name;
    wrapper["content"] = content;
    std::ostringstream os;
    os << "<tool_response>\n" << wrapper.dump() << "\n</tool_response>";
    return os.str();
}

std::string default_tools_system_prompt(const std::string& formatted_tools) {
    std::ostringstream os;
    os << "You are a function calling AI model. You are provided with function "
          "signatures within <tools> </tools> XML tags. You may call one or "
          "more functions to assist with the user query. If available tools "
          "are not relevant in assisting with user query, just respond in "
          "natural conversational language. Don't make assumptions about what "
          "values to plug into functions. After calling & executing the "
          "functions, you will be provided with function results within "
          "<tool_response> </tool_response> XML tags. Here are the available "
          "tools:\n<tools>\n"
       << formatted_tools
       << "\n</tools>\n"
          "For each function call return a JSON object, with the following "
          "pydantic model json schema for each:\n"
          "{'title': 'FunctionCall', 'type': 'object', 'properties': {'name': "
          "{'title': 'Name', 'type': 'string'}, 'arguments': {'title': "
          "'Arguments', 'type': 'object'}}, 'required': ['name', "
          "'arguments']}\n"
          "Each function call should be enclosed within <tool_call> "
          "</tool_call> XML tags.\n"
          "Example:\n<tool_call>\n{'name': <function-name>,'arguments': "
          "<args-dict>}\n</tool_call>";
    return os.str();
}

nlohmann::json to_hf_sft_conversations(const std::vector<OpenAIMessage>& messages,
                                        const std::string& user_query,
                                        const std::string& tools_system_prompt) {
    nlohmann::json conversations = nlohmann::json::array();

    if (!tools_system_prompt.empty()) {
        conversations.push_back({{"from", "system"},
                                  {"value", tools_system_prompt}});
    }

    if (!user_query.empty()) {
        conversations.push_back({{"from", "human"}, {"value", user_query}});
    }

    // Walk the messages.  We skip the first user message because it is
    // already materialised above as the seed ``human`` turn.
    bool seeded_human = !user_query.empty();

    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];

        if (msg.role == "user") {
            if (seeded_human) {
                // First user message was already added; don't duplicate.
                seeded_human = false;
                continue;
            }
            conversations.push_back({{"from", "human"}, {"value", msg.content}});

        } else if (msg.role == "assistant") {
            std::string body;
            if (!msg.reasoning.empty()) {
                body = "<think>\n" + msg.reasoning + "\n</think>\n";
            }
            if (!msg.content.empty()) {
                body += msg.content;
                if (body.back() != '\n') body += "\n";
            }
            // Append any tool calls.
            if (msg.tool_calls.is_array() && !msg.tool_calls.empty()) {
                for (const auto& tc : msg.tool_calls) {
                    if (!tc.is_object()) continue;
                    auto fn = tc.value("function", nlohmann::json::object());
                    std::string name = fn.value("name", std::string{});
                    auto args = coerce_arguments(fn.value("arguments",
                                                          nlohmann::json()));
                    body += format_tool_call(name, args);
                }
            }
            body = ensure_think_block(body);
            // Strip the trailing newline on the last tool_call to match
            // the Python output.
            while (!body.empty() && body.back() == '\n') body.pop_back();
            conversations.push_back({{"from", "gpt"}, {"value", body}});

            // Collect consecutive ``tool`` responses into a single turn.
            std::vector<std::string> tool_responses;
            std::size_t j = i + 1;
            std::size_t tc_index = 0;
            while (j < messages.size() && messages[j].role == "tool") {
                const auto& tm = messages[j];
                std::string name = "unknown";
                if (msg.tool_calls.is_array() &&
                    tc_index < msg.tool_calls.size()) {
                    name = msg.tool_calls[tc_index]
                               .value("function", nlohmann::json::object())
                               .value("name", std::string("unknown"));
                }
                // Parse tool output as JSON when it looks like JSON; else
                // keep as string.
                nlohmann::json content_json;
                const auto& c = tm.content;
                if (!c.empty() && (c.front() == '{' || c.front() == '[')) {
                    try {
                        content_json = nlohmann::json::parse(c);
                    } catch (...) {
                        content_json = c;
                    }
                } else {
                    content_json = c;
                }
                tool_responses.push_back(format_tool_response(
                    tm.tool_call_id, name, content_json));
                ++tc_index;
                ++j;
            }
            if (!tool_responses.empty()) {
                std::string joined;
                for (std::size_t k = 0; k < tool_responses.size(); ++k) {
                    if (k > 0) joined += "\n";
                    joined += tool_responses[k];
                }
                conversations.push_back({{"from", "tool"}, {"value", joined}});
                i = j - 1;  // skip over the tool messages we consumed
            }
        }
        // system messages in the OpenAI stream are ignored — the tools
        // prompt is injected at the top of the trajectory.
    }

    return conversations;
}

nlohmann::json to_hf_sft_record(const std::vector<OpenAIMessage>& messages,
                                 const std::string& user_query,
                                 const std::string& tools_system_prompt,
                                 const nlohmann::json& metadata) {
    nlohmann::json record;
    record["conversations"] = to_hf_sft_conversations(messages, user_query,
                                                      tools_system_prompt);
    if (!metadata.is_null() && !metadata.empty()) {
        record["metadata"] = metadata;
    }
    return record;
}

nlohmann::json turns_to_conversations(const std::vector<TrajectoryTurn>& turns) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& t : turns) {
        out.push_back({{"from", t.from}, {"value", t.value}});
    }
    return out;
}

}  // namespace hermes::batch
