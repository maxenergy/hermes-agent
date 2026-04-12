// Honcho AI memory provider implementation.
#include "hermes/agent/honcho_provider.hpp"

#include <nlohmann/json.hpp>

namespace hermes::agent {

HonchoMemoryProvider::HonchoMemoryProvider(Config cfg,
                                           hermes::llm::HttpTransport* t)
    : cfg_(std::move(cfg)), transport_(t) {}

HonchoMemoryProvider::HonchoMemoryProvider(Config cfg)
    : cfg_(std::move(cfg)),
      transport_(hermes::llm::get_default_transport()) {}

bool HonchoMemoryProvider::config_valid() const {
    return !cfg_.api_url.empty() && !cfg_.api_key.empty() &&
           !cfg_.app_id.empty() && !cfg_.user_id.empty();
}

std::string HonchoMemoryProvider::base_path() const {
    return cfg_.api_url + "/apps/" + cfg_.app_id + "/users/" + cfg_.user_id;
}

std::string HonchoMemoryProvider::build_system_prompt_section() {
    if (cached_insights_.empty()) return {};
    std::string out = "## User Context\n";
    for (const auto& insight : cached_insights_) {
        out += "- ";
        out += insight;
        out += '\n';
    }
    return out;
}

void HonchoMemoryProvider::ensure_session() {
    if (!session_id_.empty()) return;
    if (!transport_ || !config_valid()) return;

    try {
        auto resp = transport_->post_json(
            base_path() + "/sessions",
            {{"Authorization", "Bearer " + cfg_.api_key},
             {"Content-Type", "application/json"}},
            "{}");
        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (!body.is_discarded() && body.contains("id")) {
                session_id_ = body["id"].get<std::string>();
            }
        }
    } catch (...) {
        // Swallow — provider degrades gracefully.
    }
}

void HonchoMemoryProvider::prefetch(std::string_view /*user_message*/) {
    if (!transport_ || !config_valid()) return;

    try {
        auto resp = transport_->get(
            base_path() + "/insights",
            {{"Authorization", "Bearer " + cfg_.api_key}});
        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto body = nlohmann::json::parse(resp.body, nullptr, false);
            if (body.is_discarded()) return;

            cached_insights_.clear();
            // Accept either {"insights": [...]} or a bare array.
            const auto& arr = body.contains("insights") && body["insights"].is_array()
                                  ? body["insights"]
                                  : body;
            if (arr.is_array()) {
                for (const auto& item : arr) {
                    if (item.is_string()) {
                        cached_insights_.push_back(item.get<std::string>());
                    } else if (item.is_object() && item.contains("text")) {
                        cached_insights_.push_back(
                            item["text"].get<std::string>());
                    } else if (item.is_object() && item.contains("content")) {
                        cached_insights_.push_back(
                            item["content"].get<std::string>());
                    }
                }
            }
        }
    } catch (...) {
        // Ignore; keep stale cache.
    }
}

void HonchoMemoryProvider::sync(std::string_view user_msg,
                                std::string_view assistant_response) {
    if (!transport_ || !config_valid()) return;
    ensure_session();
    if (session_id_.empty()) return;

    auto post_message = [&](const std::string& role, std::string_view content) {
        nlohmann::json payload = {
            {"role", role},
            {"content", std::string(content)},
        };
        try {
            transport_->post_json(
                base_path() + "/sessions/" + session_id_ + "/messages",
                {{"Authorization", "Bearer " + cfg_.api_key},
                 {"Content-Type", "application/json"}},
                payload.dump());
        } catch (...) {
        }
    };
    post_message("user", user_msg);
    post_message("assistant", assistant_response);
}

}  // namespace hermes::agent
