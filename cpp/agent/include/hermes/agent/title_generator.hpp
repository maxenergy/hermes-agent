// Auto-generate short session titles from the first exchange.
//
// C++17 port of agent/title_generator.py. Runs the aux LLM client on
// the first user/assistant pair and stores the result via an injected
// SessionTitleStore callback. maybe_auto_title_async() spawns a
// detached background thread so title generation never blocks the
// user-visible reply.
#pragma once

#include "hermes/llm/llm_client.hpp"
#include "hermes/llm/message.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hermes::agent {

// Store callbacks — the caller wires these to the real SessionDB in
// hermes_state. The wrapper keeps title_generator free of SessionDB
// coupling so it can be unit-tested with mocks.
struct SessionTitleStore {
    // Return the currently-set title for session_id, or empty string.
    std::function<std::string(const std::string& session_id)> get_title;
    // Persist a new title. Return true on success.
    std::function<bool(const std::string& session_id,
                       const std::string& title)> set_title;
};

// Call the auxiliary client and return a cleaned-up title, or empty
// string on failure. Applied cleanups: strip surrounding quotes, drop
// leading "Title:" prefix, cap at 80 chars.
std::string generate_title(hermes::llm::LlmClient* aux,
                           std::string_view model,
                           std::string_view first_user_message,
                           std::string_view first_assistant_response = {});

// Synchronous title-and-persist helper. Skips when the session already
// has a title, when the store callbacks are missing, or when title
// generation fails.
void auto_title_session(const SessionTitleStore& store,
                        hermes::llm::LlmClient* aux,
                        const std::string& model,
                        const std::string& session_id,
                        const std::string& user_message,
                        const std::string& assistant_response);

// Heuristic: generate a title only on the first 1-2 user exchanges.
// Returns true if generation would run.
bool should_auto_title(const std::vector<hermes::llm::Message>& conversation_history);

// Fire-and-forget: spawns a detached std::thread to run
// auto_title_session(). Returns immediately. No-op if should_auto_title
// returns false or any required argument is empty/null.
void maybe_auto_title_async(
    const SessionTitleStore& store,
    hermes::llm::LlmClient* aux,
    const std::string& model,
    const std::string& session_id,
    const std::string& user_message,
    const std::string& assistant_response,
    const std::vector<hermes::llm::Message>& conversation_history);

// Cleanup helper exposed for tests: trim whitespace, strip quotes, drop
// leading "Title:" prefix, cap length.
std::string postprocess_title(std::string title);

}  // namespace hermes::agent
