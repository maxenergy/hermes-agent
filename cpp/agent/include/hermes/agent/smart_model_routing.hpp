// Optional cheap-vs-strong model routing.
//
// C++17 port of agent/smart_model_routing.py. Inspects the next user turn
// and — if it looks "simple" — returns the configured cheap-model route.
// Deliberately conservative: code fences, URLs, multi-line messages, and
// keywords that hint at debugging / tool use all force the primary model.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace hermes::agent {

struct CheapModelRoute {
    std::string provider;
    std::string model;
    std::string api_key_env;
    std::string base_url;
    std::string routing_reason;  // always "simple_turn" when returned
    // Original config dict preserved for downstream consumers.
    nlohmann::json extras;
};

// Parse routing config and the incoming user message. Returns std::nullopt
// when the message should stay on the primary model.
std::optional<CheapModelRoute> choose_cheap_model_route(
    const std::string& user_message,
    const nlohmann::json& routing_config);

namespace detail {

// Exposed for tests.
bool message_matches_complex_keyword(const std::string& lowered);
bool contains_url(const std::string& text);

}  // namespace detail

}  // namespace hermes::agent
