// C++17 port of agent/prompt_caching.py.
#include "hermes/agent/prompt_caching.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace hermes::agent {

namespace {

void apply_cache_marker(nlohmann::json& msg,
                        const nlohmann::json& marker,
                        bool native_anthropic) {
    std::string role = msg.value("role", std::string());
    auto it = msg.find("content");

    if (role == "tool") {
        if (native_anthropic) {
            msg["cache_control"] = marker;
        }
        return;
    }

    const bool empty_content =
        (it == msg.end()) || it->is_null() ||
        (it->is_string() && it->get<std::string>().empty());
    if (empty_content) {
        msg["cache_control"] = marker;
        return;
    }

    if (it->is_string()) {
        nlohmann::json replacement = nlohmann::json::array();
        replacement.push_back({
            {"type", "text"},
            {"text", it->get<std::string>()},
            {"cache_control", marker},
        });
        msg["content"] = std::move(replacement);
        return;
    }

    if (it->is_array() && !it->empty()) {
        nlohmann::json& last = it->back();
        if (last.is_object()) {
            last["cache_control"] = marker;
        }
    }
}

}  // namespace

nlohmann::json apply_anthropic_cache_control(const nlohmann::json& api_messages,
                                             const std::string& cache_ttl,
                                             bool native_anthropic) {
    // Deep-copy semantics — nlohmann::json copy ctor is deep by value.
    nlohmann::json messages = api_messages;
    if (!messages.is_array() || messages.empty()) {
        return messages;
    }

    nlohmann::json marker = {{"type", "ephemeral"}};
    if (cache_ttl == "1h") {
        marker["ttl"] = "1h";
    }

    int breakpoints_used = 0;
    if (messages[0].is_object() && messages[0].value("role", std::string()) == "system") {
        apply_cache_marker(messages[0], marker, native_anthropic);
        breakpoints_used += 1;
    }

    const int remaining = 4 - breakpoints_used;
    if (remaining <= 0) {
        return messages;
    }

    // Collect indices of non-system messages.
    std::vector<std::size_t> non_sys;
    non_sys.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (!messages[i].is_object() ||
            messages[i].value("role", std::string()) != "system") {
            non_sys.push_back(i);
        }
    }

    const std::size_t take = std::min(static_cast<std::size_t>(remaining), non_sys.size());
    const std::size_t start = non_sys.size() - take;
    for (std::size_t k = start; k < non_sys.size(); ++k) {
        apply_cache_marker(messages[non_sys[k]], marker, native_anthropic);
    }

    return messages;
}

}  // namespace hermes::agent
