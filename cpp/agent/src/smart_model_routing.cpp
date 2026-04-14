// C++17 port of agent/smart_model_routing.py.
#include "hermes/agent/smart_model_routing.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

namespace hermes::agent {

namespace {

const std::unordered_set<std::string>& complex_keywords() {
    static const std::unordered_set<std::string> kw = {
        "debug", "debugging", "implement", "implementation", "refactor",
        "patch", "traceback", "stacktrace", "exception", "error",
        "analyze", "analysis", "investigate", "architecture", "design",
        "compare", "benchmark", "optimize", "optimise", "review",
        "terminal", "shell", "tool", "tools", "pytest", "test", "tests",
        "plan", "planning", "delegate", "subagent", "cron", "docker",
        "kubernetes",
    };
    return kw;
}

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string strip_punctuation(const std::string& token) {
    static const std::string punct = ".,:;!?()[]{}\"'`";
    std::size_t b = 0, e = token.size();
    while (b < e && punct.find(token[b]) != std::string::npos) ++b;
    while (e > b && punct.find(token[e - 1]) != std::string::npos) --e;
    return token.substr(b, e - b);
}

std::string strip_space(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

int word_count(const std::string& text) {
    int n = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++n;
        }
    }
    return n;
}

int newline_count(const std::string& text) {
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

bool coerce_bool(const nlohmann::json& v, bool def = false) {
    if (v.is_null()) return def;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<long long>() != 0;
    if (v.is_number_float()) return v.get<double>() != 0.0;
    if (v.is_string()) {
        std::string s = lower_copy(v.get<std::string>());
        if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "y") return true;
        if (s == "0" || s == "false" || s == "no" || s == "off" || s == "n" || s.empty())
            return false;
        return def;
    }
    return def;
}

int coerce_int(const nlohmann::json& v, int def) {
    if (v.is_number_integer()) return static_cast<int>(v.get<long long>());
    if (v.is_number_float()) return static_cast<int>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) { return def; }
    }
    return def;
}

std::string get_str(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

namespace detail {

bool message_matches_complex_keyword(const std::string& lowered) {
    const auto& kw = complex_keywords();
    // Tokenize on whitespace, strip punctuation from each token.
    std::string buf;
    auto check = [&](const std::string& tok) {
        return kw.find(tok) != kw.end();
    };
    for (unsigned char c : lowered) {
        if (std::isspace(c)) {
            if (!buf.empty()) {
                if (check(strip_punctuation(buf))) return true;
                buf.clear();
            }
        } else {
            buf.push_back(static_cast<char>(c));
        }
    }
    if (!buf.empty() && check(strip_punctuation(buf))) return true;
    return false;
}

bool contains_url(const std::string& text) {
    static const std::regex url_re(R"(https?://|www\.)", std::regex::icase);
    return std::regex_search(text, url_re);
}

}  // namespace detail

std::optional<CheapModelRoute> choose_cheap_model_route(
    const std::string& user_message,
    const nlohmann::json& routing_config) {
    if (!routing_config.is_object()) return std::nullopt;

    auto enabled_it = routing_config.find("enabled");
    const bool enabled = enabled_it != routing_config.end()
                             ? coerce_bool(*enabled_it, false)
                             : false;
    if (!enabled) return std::nullopt;

    auto cm_it = routing_config.find("cheap_model");
    if (cm_it == routing_config.end() || !cm_it->is_object()) return std::nullopt;
    const nlohmann::json& cheap_model = *cm_it;

    std::string provider = get_str(cheap_model, "provider");
    provider = strip_space(lower_copy(provider));
    std::string model = strip_space(get_str(cheap_model, "model"));
    if (provider.empty() || model.empty()) return std::nullopt;

    const std::string text = strip_space(user_message);
    if (text.empty()) return std::nullopt;

    const int max_chars = coerce_int(
        routing_config.value("max_simple_chars", nlohmann::json()), 160);
    const int max_words = coerce_int(
        routing_config.value("max_simple_words", nlohmann::json()), 28);

    if (static_cast<int>(text.size()) > max_chars) return std::nullopt;
    if (word_count(text) > max_words) return std::nullopt;
    if (newline_count(text) > 1) return std::nullopt;
    if (text.find("```") != std::string::npos) return std::nullopt;
    if (text.find('`') != std::string::npos) return std::nullopt;
    if (detail::contains_url(text)) return std::nullopt;

    const std::string lowered = lower_copy(text);
    if (detail::message_matches_complex_keyword(lowered)) return std::nullopt;

    CheapModelRoute route;
    route.provider = provider;
    route.model = model;
    route.api_key_env = get_str(cheap_model, "api_key_env");
    route.base_url = get_str(cheap_model, "base_url");
    route.routing_reason = "simple_turn";
    route.extras = cheap_model;
    return route;
}

}  // namespace hermes::agent
