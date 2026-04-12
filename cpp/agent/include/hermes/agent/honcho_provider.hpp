// Honcho AI memory provider — uses https://honcho.dev user modeling
// API to persist long-term user context and fetch dialectic insights
// that are injected into the system prompt.
#pragma once

#include "hermes/agent/memory_provider.hpp"
#include "hermes/llm/llm_client.hpp"

#include <string>
#include <vector>

namespace hermes::agent {

class HonchoMemoryProvider : public MemoryProvider {
public:
    struct Config {
        std::string api_url = "https://api.honcho.dev/v1";
        std::string api_key;  // $HONCHO_API_KEY
        std::string app_id;
        std::string user_id;
    };

    // Transport may be null — in which case prefetch/sync become no-ops
    // and build_system_prompt_section returns the empty string.  A
    // FakeHttpTransport may be injected for unit tests.
    HonchoMemoryProvider(Config cfg, hermes::llm::HttpTransport* transport);
    explicit HonchoMemoryProvider(Config cfg);

    std::string name() const override { return "honcho"; }
    bool is_external() const override { return true; }
    std::string build_system_prompt_section() override;
    void prefetch(std::string_view user_message) override;
    void sync(std::string_view user_msg,
              std::string_view assistant_response) override;

    // True if the config is complete enough to actually talk to Honcho.
    bool config_valid() const;

    // Accessors for tests.
    const Config& config() const { return cfg_; }
    const std::string& session_id() const { return session_id_; }
    const std::vector<std::string>& cached_insights() const {
        return cached_insights_;
    }

    // Seed cached insights (test helper).
    void set_cached_insights_for_test(std::vector<std::string> insights) {
        cached_insights_ = std::move(insights);
    }

private:
    Config cfg_;
    hermes::llm::HttpTransport* transport_ = nullptr;
    std::string session_id_;
    std::vector<std::string> cached_insights_;

    void ensure_session();
    std::string base_path() const;  // "/apps/{app}/users/{user}"
};

}  // namespace hermes::agent
