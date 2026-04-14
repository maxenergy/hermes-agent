// C++17 port of agent/error_classifier.py.
#include "hermes/agent/error_classifier.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string>
#include <unordered_set>

namespace hermes::agent::errclass {

// ── Pattern tables ────────────────────────────────────────────────────

namespace {

constexpr const char* k_billing_patterns[] = {
    "insufficient credits",
    "insufficient_quota",
    "credit balance",
    "credits have been exhausted",
    "top up your credits",
    "payment required",
    "billing hard limit",
    "exceeded your current quota",
    "account is deactivated",
    "plan does not include",
};

constexpr const char* k_rate_limit_patterns[] = {
    "rate limit",
    "rate_limit",
    "too many requests",
    "throttled",
    "requests per minute",
    "tokens per minute",
    "requests per day",
    "try again in",
    "please retry after",
    "resource_exhausted",
    "rate increased too quickly",
};

constexpr const char* k_usage_limit_patterns[] = {
    "usage limit",
    "quota",
    "limit exceeded",
    "key limit exceeded",
};

constexpr const char* k_usage_limit_transient_signals[] = {
    "try again",
    "retry",
    "resets at",
    "reset in",
    "wait",
    "requests remaining",
    "periodic",
    "window",
};

constexpr const char* k_payload_too_large_patterns[] = {
    "request entity too large",
    "payload too large",
    "error code: 413",
};

constexpr const char* k_context_overflow_patterns[] = {
    "context length",
    "context size",
    "maximum context",
    "token limit",
    "too many tokens",
    "reduce the length",
    "exceeds the limit",
    "context window",
    "prompt is too long",
    "prompt exceeds max length",
    "max_tokens",
    "maximum number of tokens",
    u8"超过最大长度",
    u8"上下文长度",
};

constexpr const char* k_model_not_found_patterns[] = {
    "is not a valid model",
    "invalid model",
    "model not found",
    "model_not_found",
    "does not exist",
    "no such model",
    "unknown model",
    "unsupported model",
};

constexpr const char* k_auth_patterns[] = {
    "invalid api key",
    "invalid_api_key",
    "authentication",
    "unauthorized",
    "forbidden",
    "invalid token",
    "token expired",
    "token revoked",
    "access denied",
};

constexpr const char* k_server_disconnect_patterns[] = {
    "server disconnected",
    "peer closed connection",
    "connection reset by peer",
    "connection was closed",
    "network connection lost",
    "unexpected eof",
    "incomplete chunked read",
};

const std::unordered_set<std::string>& transport_error_types() {
    static const std::unordered_set<std::string> s = {
        "ReadTimeout", "ConnectTimeout", "PoolTimeout",
        "ConnectError", "RemoteProtocolError",
        "ConnectionError", "ConnectionResetError",
        "ConnectionAbortedError", "BrokenPipeError",
        "TimeoutError", "ReadError",
        "ServerDisconnectedError",
        "APIConnectionError", "APITimeoutError",
    };
    return s;
}

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

template <std::size_t N>
bool contains_any(const std::string& lowered, const char* const (&patterns)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (lowered.find(patterns[i]) != std::string::npos) return true;
    }
    return false;
}

std::string get_json_string(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

}  // namespace

// ── Public helpers ────────────────────────────────────────────────────

std::string to_string(FailoverReason r) {
    switch (r) {
        case FailoverReason::auth: return "auth";
        case FailoverReason::auth_permanent: return "auth_permanent";
        case FailoverReason::billing: return "billing";
        case FailoverReason::rate_limit: return "rate_limit";
        case FailoverReason::overloaded: return "overloaded";
        case FailoverReason::server_error: return "server_error";
        case FailoverReason::timeout: return "timeout";
        case FailoverReason::context_overflow: return "context_overflow";
        case FailoverReason::payload_too_large: return "payload_too_large";
        case FailoverReason::model_not_found: return "model_not_found";
        case FailoverReason::format_error: return "format_error";
        case FailoverReason::thinking_signature: return "thinking_signature";
        case FailoverReason::long_context_tier: return "long_context_tier";
        case FailoverReason::unknown: return "unknown";
    }
    return "unknown";
}

namespace detail {

std::string extract_error_code(const nlohmann::json& body) {
    if (!body.is_object() || body.empty()) return {};
    auto err_it = body.find("error");
    if (err_it != body.end() && err_it->is_object()) {
        std::string code = get_json_string(*err_it, "code");
        if (code.empty()) code = get_json_string(*err_it, "type");
        code = trim(code);
        if (!code.empty()) return code;
    }
    std::string top = get_json_string(body, "code");
    if (top.empty()) top = get_json_string(body, "error_code");
    return trim(top);
}

std::string extract_message(const std::string& raw, const nlohmann::json& body) {
    auto clip = [](std::string s) {
        if (s.size() > 500) s.resize(500);
        return s;
    };
    if (body.is_object() && !body.empty()) {
        auto err_it = body.find("error");
        if (err_it != body.end() && err_it->is_object()) {
            std::string msg = get_json_string(*err_it, "message");
            msg = trim(msg);
            if (!msg.empty()) return clip(msg);
        }
        std::string msg = trim(get_json_string(body, "message"));
        if (!msg.empty()) return clip(msg);
    }
    return clip(raw);
}

std::string build_error_msg(const std::string& raw, const nlohmann::json& body) {
    std::string raw_msg = lower_copy(raw);
    std::string body_msg;
    std::string meta_msg;
    if (body.is_object() && !body.empty()) {
        auto err_it = body.find("error");
        if (err_it != body.end() && err_it->is_object()) {
            body_msg = lower_copy(get_json_string(*err_it, "message"));
            auto meta_it = err_it->find("metadata");
            if (meta_it != err_it->end() && meta_it->is_object()) {
                std::string raw_json = get_json_string(*meta_it, "raw");
                if (!trim(raw_json).empty()) {
                    try {
                        auto inner = nlohmann::json::parse(raw_json);
                        if (inner.is_object()) {
                            auto inner_err = inner.find("error");
                            if (inner_err != inner.end() && inner_err->is_object()) {
                                meta_msg = lower_copy(get_json_string(*inner_err, "message"));
                            }
                        }
                    } catch (...) {}
                }
            }
        }
        if (body_msg.empty()) body_msg = lower_copy(get_json_string(body, "message"));
    }
    std::string combined = raw_msg;
    if (!body_msg.empty() && raw_msg.find(body_msg) == std::string::npos) {
        combined += ' '; combined += body_msg;
    }
    if (!meta_msg.empty() &&
        combined.find(meta_msg) == std::string::npos) {
        combined += ' '; combined += meta_msg;
    }
    return combined;
}

bool any_of_patterns(const std::string& lowered_text,
                     const std::initializer_list<const char*>& patterns) {
    for (const char* p : patterns) {
        if (lowered_text.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace detail

// ── Pipeline internals ────────────────────────────────────────────────

namespace {

ClassifiedError make_result(FailoverReason reason, const ErrorInfo& info,
                            const nlohmann::json& body,
                            bool retryable = true,
                            bool should_compress = false,
                            bool should_rotate_credential = false,
                            bool should_fallback = false) {
    ClassifiedError ce;
    ce.reason = reason;
    ce.status_code = info.status_code;
    ce.provider = info.provider;
    ce.model = info.model;
    ce.message = detail::extract_message(info.raw_message, body);
    ce.retryable = retryable;
    ce.should_compress = should_compress;
    ce.should_rotate_credential = should_rotate_credential;
    ce.should_fallback = should_fallback;
    return ce;
}

std::optional<ClassifiedError> classify_402(const std::string& error_msg,
                                            const ErrorInfo& info,
                                            const nlohmann::json& body) {
    const bool has_usage_limit = contains_any(error_msg, k_usage_limit_patterns);
    const bool has_transient = contains_any(error_msg, k_usage_limit_transient_signals);
    if (has_usage_limit && has_transient) {
        return make_result(FailoverReason::rate_limit, info, body,
                           /*retry=*/true, false, true, true);
    }
    return make_result(FailoverReason::billing, info, body,
                       /*retry=*/false, false, true, true);
}

std::optional<ClassifiedError> classify_400(const std::string& error_msg,
                                            const ErrorInfo& info,
                                            const nlohmann::json& body) {
    if (contains_any(error_msg, k_context_overflow_patterns)) {
        return make_result(FailoverReason::context_overflow, info, body,
                           true, true, false, false);
    }
    if (contains_any(error_msg, k_model_not_found_patterns)) {
        return make_result(FailoverReason::model_not_found, info, body,
                           false, false, false, true);
    }
    if (contains_any(error_msg, k_rate_limit_patterns)) {
        return make_result(FailoverReason::rate_limit, info, body,
                           true, false, true, true);
    }
    if (contains_any(error_msg, k_billing_patterns)) {
        return make_result(FailoverReason::billing, info, body,
                           false, false, true, true);
    }
    // Generic 400 + large session → probable context overflow.
    std::string err_body_msg;
    if (body.is_object()) {
        auto err_it = body.find("error");
        if (err_it != body.end() && err_it->is_object()) {
            err_body_msg = lower_copy(trim(get_json_string(*err_it, "message")));
        }
        if (err_body_msg.empty()) {
            err_body_msg = lower_copy(trim(get_json_string(body, "message")));
        }
    }
    const bool is_generic = err_body_msg.size() < 30 ||
                            err_body_msg == "error" || err_body_msg.empty();
    const bool is_large = info.approx_tokens > info.context_length * 4 / 10 ||
                          info.approx_tokens > 80000 || info.num_messages > 80;
    if (is_generic && is_large) {
        return make_result(FailoverReason::context_overflow, info, body,
                           true, true, false, false);
    }
    return make_result(FailoverReason::format_error, info, body,
                       false, false, false, true);
}

std::optional<ClassifiedError> classify_by_status(int status_code,
                                                  const std::string& error_msg,
                                                  const ErrorInfo& info,
                                                  const nlohmann::json& body) {
    if (status_code == 401) {
        return make_result(FailoverReason::auth, info, body,
                           false, false, true, true);
    }
    if (status_code == 403) {
        if (error_msg.find("key limit exceeded") != std::string::npos ||
            error_msg.find("spending limit") != std::string::npos) {
            return make_result(FailoverReason::billing, info, body,
                               false, false, true, true);
        }
        return make_result(FailoverReason::auth, info, body,
                           false, false, false, true);
    }
    if (status_code == 402) return classify_402(error_msg, info, body);
    if (status_code == 404) {
        return make_result(FailoverReason::model_not_found, info, body,
                           false, false, false, true);
    }
    if (status_code == 413) {
        return make_result(FailoverReason::payload_too_large, info, body,
                           true, true, false, false);
    }
    if (status_code == 429) {
        return make_result(FailoverReason::rate_limit, info, body,
                           true, false, true, true);
    }
    if (status_code == 400) return classify_400(error_msg, info, body);
    if (status_code == 500 || status_code == 502) {
        return make_result(FailoverReason::server_error, info, body, true);
    }
    if (status_code == 503 || status_code == 529) {
        return make_result(FailoverReason::overloaded, info, body, true);
    }
    if (status_code >= 400 && status_code < 500) {
        return make_result(FailoverReason::format_error, info, body,
                           false, false, false, true);
    }
    if (status_code >= 500 && status_code < 600) {
        return make_result(FailoverReason::server_error, info, body, true);
    }
    return std::nullopt;
}

std::optional<ClassifiedError> classify_by_error_code(
    const std::string& error_code,
    const ErrorInfo& info,
    const nlohmann::json& body) {
    const std::string code = lower_copy(error_code);
    if (code == "resource_exhausted" || code == "throttled" ||
        code == "rate_limit_exceeded") {
        return make_result(FailoverReason::rate_limit, info, body,
                           true, false, true, false);
    }
    if (code == "insufficient_quota" || code == "billing_not_active" ||
        code == "payment_required") {
        return make_result(FailoverReason::billing, info, body,
                           false, false, true, true);
    }
    if (code == "model_not_found" || code == "model_not_available" ||
        code == "invalid_model") {
        return make_result(FailoverReason::model_not_found, info, body,
                           false, false, false, true);
    }
    if (code == "context_length_exceeded" || code == "max_tokens_exceeded") {
        return make_result(FailoverReason::context_overflow, info, body,
                           true, true, false, false);
    }
    return std::nullopt;
}

std::optional<ClassifiedError> classify_by_message(
    const std::string& error_msg,
    const ErrorInfo& info,
    const nlohmann::json& body) {
    if (contains_any(error_msg, k_payload_too_large_patterns)) {
        return make_result(FailoverReason::payload_too_large, info, body,
                           true, true, false, false);
    }
    const bool has_usage_limit = contains_any(error_msg, k_usage_limit_patterns);
    if (has_usage_limit) {
        const bool has_transient = contains_any(error_msg, k_usage_limit_transient_signals);
        if (has_transient) {
            return make_result(FailoverReason::rate_limit, info, body,
                               true, false, true, true);
        }
        return make_result(FailoverReason::billing, info, body,
                           false, false, true, true);
    }
    if (contains_any(error_msg, k_billing_patterns)) {
        return make_result(FailoverReason::billing, info, body,
                           false, false, true, true);
    }
    if (contains_any(error_msg, k_rate_limit_patterns)) {
        return make_result(FailoverReason::rate_limit, info, body,
                           true, false, true, true);
    }
    if (contains_any(error_msg, k_context_overflow_patterns)) {
        return make_result(FailoverReason::context_overflow, info, body,
                           true, true, false, false);
    }
    if (contains_any(error_msg, k_auth_patterns)) {
        return make_result(FailoverReason::auth, info, body,
                           false, false, true, true);
    }
    if (contains_any(error_msg, k_model_not_found_patterns)) {
        return make_result(FailoverReason::model_not_found, info, body,
                           false, false, false, true);
    }
    return std::nullopt;
}

}  // namespace

// ── Public entry point ────────────────────────────────────────────────

ClassifiedError classify_api_error(const ErrorInfo& info) {
    const nlohmann::json& body = info.body;
    const std::string error_code = detail::extract_error_code(body);
    const std::string error_msg = detail::build_error_msg(info.raw_message, body);

    // 1. Provider-specific patterns (highest priority).
    if (info.status_code == 400 &&
        error_msg.find("signature") != std::string::npos &&
        error_msg.find("thinking") != std::string::npos) {
        return make_result(FailoverReason::thinking_signature, info, body,
                           true, false, false, false);
    }
    if (info.status_code == 429 &&
        error_msg.find("extra usage") != std::string::npos &&
        error_msg.find("long context") != std::string::npos) {
        return make_result(FailoverReason::long_context_tier, info, body,
                           true, true, false, false);
    }

    // 2. HTTP status code classification.
    if (info.status_code.has_value()) {
        auto r = classify_by_status(*info.status_code, error_msg, info, body);
        if (r.has_value()) return *r;
    }

    // 3. Error code classification.
    if (!error_code.empty()) {
        auto r = classify_by_error_code(error_code, info, body);
        if (r.has_value()) return *r;
    }

    // 4. Message pattern matching.
    if (auto r = classify_by_message(error_msg, info, body); r.has_value()) {
        return *r;
    }

    // 5. Server disconnect + large session → context overflow.
    const bool is_disconnect = contains_any(error_msg, k_server_disconnect_patterns);
    if (is_disconnect && !info.status_code.has_value()) {
        const bool is_large =
            info.approx_tokens > info.context_length * 6 / 10 ||
            info.approx_tokens > 120000 || info.num_messages > 200;
        if (is_large) {
            return make_result(FailoverReason::context_overflow, info, body,
                               true, true, false, false);
        }
        return make_result(FailoverReason::timeout, info, body, true);
    }

    // 6. Transport / timeout heuristics.
    if (transport_error_types().count(info.error_type)) {
        return make_result(FailoverReason::timeout, info, body, true);
    }

    // 7. Fallback: unknown, retryable with backoff.
    return make_result(FailoverReason::unknown, info, body, true);
}

}  // namespace hermes::agent::errclass
