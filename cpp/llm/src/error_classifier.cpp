// Simplified API error classifier — only the cases Phase 3 explicitly
// drives on.  See error_classifier.hpp for the reason taxonomy.
#include "hermes/llm/error_classifier.hpp"

#include "hermes/llm/model_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>

namespace hermes::llm {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::optional<std::chrono::seconds> parse_retry_after(
    const std::unordered_map<std::string, std::string>& headers) {
    // Look up both canonical casings.
    const std::string* value = nullptr;
    for (const auto& kv : headers) {
        std::string key = to_lower(kv.first);
        if (key == "retry-after") {
            value = &kv.second;
            break;
        }
    }
    if (value && !value->empty()) {
        int secs = 0;
        auto [ptr, ec] = std::from_chars(value->data(),
                                         value->data() + value->size(),
                                         secs);
        if (ec == std::errc() && secs >= 0) {
            return std::chrono::seconds(secs);
        }
    }
    // retry-after-ms (OpenAI).
    for (const auto& kv : headers) {
        if (to_lower(kv.first) == "retry-after-ms") {
            int ms = 0;
            auto [ptr, ec] = std::from_chars(kv.second.data(),
                                             kv.second.data() + kv.second.size(),
                                             ms);
            if (ec == std::errc() && ms >= 0) {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::milliseconds(ms));
            }
        }
    }
    return std::nullopt;
}

}  // namespace

ClassifiedError classify_api_error(
    int status_code,
    std::string_view body,
    const std::unordered_map<std::string, std::string>& headers) {
    ClassifiedError out;
    const std::string lower_body = to_lower(body);
    out.detail = std::string(body.substr(0, std::min<size_t>(body.size(), 500)));

    // 429 → rate limit (check BEFORE generic 5xx since 429 could be wrapped).
    if (status_code == 429) {
        out.reason = FailoverReason::RateLimit;
        out.retry_after = parse_retry_after(headers);
        return out;
    }

    // 408 → timeout
    if (status_code == 408) {
        out.reason = FailoverReason::Timeout;
        return out;
    }

    // 413 → payload/context overflow
    if (status_code == 413) {
        out.reason = FailoverReason::ContextOverflow;
        out.context_limit_hint = parse_context_limit_from_error(body);
        return out;
    }

    // Context overflow detected from body content (regardless of status).
    if (contains(lower_body, "context") &&
        (contains(lower_body, "length") || contains(lower_body, "window") ||
         contains(lower_body, "size"))) {
        out.reason = FailoverReason::ContextOverflow;
        out.context_limit_hint = parse_context_limit_from_error(body);
        return out;
    }
    if (contains(lower_body, "maximum context") ||
        contains(lower_body, "context_length_exceeded") ||
        contains(lower_body, "max_tokens")) {
        out.reason = FailoverReason::ContextOverflow;
        out.context_limit_hint = parse_context_limit_from_error(body);
        return out;
    }

    if (status_code == 401 || status_code == 403) {
        out.reason = FailoverReason::Unauthorized;
        return out;
    }

    if (status_code == 503) {
        out.reason = FailoverReason::ModelUnavailable;
        out.retry_after = parse_retry_after(headers);
        return out;
    }

    // model not found signals.
    if (contains(lower_body, "model_not_found") ||
        contains(lower_body, "invalid model") ||
        contains(lower_body, "model not found") ||
        contains(lower_body, "no such model")) {
        out.reason = FailoverReason::ModelUnavailable;
        return out;
    }

    if (status_code >= 500 && status_code < 600) {
        out.reason = FailoverReason::ServerError;
        return out;
    }

    if (status_code == 0) {
        out.reason = FailoverReason::NetworkError;
        return out;
    }

    out.reason = FailoverReason::Unknown;
    return out;
}

}  // namespace hermes::llm
