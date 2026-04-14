// C++17 port of agent/title_generator.py.
#include "hermes/agent/title_generator.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace hermes::agent {

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string strip_quotes(std::string s) {
    while (s.size() >= 2 && (s.front() == '"' || s.front() == '\'')) {
        if (s.back() == s.front()) {
            s = s.substr(1, s.size() - 2);
            continue;
        }
        break;
    }
    return s;
}

std::string lower_prefix(const std::string& s, std::size_t n) {
    std::string out = s.substr(0, std::min(n, s.size()));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string clip(std::string s, std::size_t max_chars = 500) {
    if (s.size() > max_chars) s.resize(max_chars);
    return s;
}

}  // namespace

std::string postprocess_title(std::string title) {
    title = trim(std::move(title));
    title = trim(strip_quotes(std::move(title)));
    if (lower_prefix(title, 6) == "title:") {
        title = trim(title.substr(6));
    }
    if (title.size() > 80) {
        title = title.substr(0, 77) + "...";
    }
    return title;
}

std::string generate_title(hermes::llm::LlmClient* aux,
                           std::string_view model,
                           std::string_view first_user_message,
                           std::string_view first_assistant_response) {
    if (!aux || model.empty() || first_user_message.empty()) return {};

    using hermes::llm::CompletionRequest;
    using hermes::llm::Message;
    using hermes::llm::Role;

    Message system_msg;
    system_msg.role = Role::System;
    system_msg.content_text =
        "Generate a short, descriptive title (3-7 words) for a conversation "
        "that starts with the following exchange. The title should capture "
        "the main topic or intent. Return ONLY the title text, nothing "
        "else. No quotes, no punctuation at the end, no prefixes.";

    std::string user_snippet = clip(std::string(first_user_message));
    std::string asst_snippet = clip(std::string(first_assistant_response));

    Message user_msg;
    user_msg.role = Role::User;
    if (!asst_snippet.empty()) {
        user_msg.content_text = "User: " + user_snippet +
                                "\n\nAssistant: " + asst_snippet;
    } else {
        user_msg.content_text = user_snippet;
    }

    CompletionRequest req;
    req.model = std::string(model);
    req.messages = {std::move(system_msg), std::move(user_msg)};
    req.max_tokens = 30;
    req.temperature = 0.3;

    try {
        auto resp = aux->complete(req);
        std::string title = trim(resp.assistant_message.content_text);
        if (title.empty() && !resp.assistant_message.content_blocks.empty()) {
            for (const auto& b : resp.assistant_message.content_blocks) {
                if (b.type == "text") {
                    title = trim(b.text);
                    if (!title.empty()) break;
                }
            }
        }
        return postprocess_title(std::move(title));
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

void auto_title_session(const SessionTitleStore& store,
                        hermes::llm::LlmClient* aux,
                        const std::string& model,
                        const std::string& session_id,
                        const std::string& user_message,
                        const std::string& assistant_response) {
    if (session_id.empty() || !store.get_title || !store.set_title) return;

    // Skip if title already exists.
    std::string existing;
    try {
        existing = store.get_title(session_id);
    } catch (...) {
        return;
    }
    if (!existing.empty()) return;

    std::string title = generate_title(aux, model, user_message, assistant_response);
    if (title.empty()) return;

    try {
        store.set_title(session_id, title);
    } catch (...) {
        // Silently swallow — title generation must never crash the main loop.
    }
}

bool should_auto_title(
    const std::vector<hermes::llm::Message>& conversation_history) {
    int user_count = 0;
    for (const auto& m : conversation_history) {
        if (m.role == hermes::llm::Role::User) ++user_count;
    }
    return user_count <= 2;
}

void maybe_auto_title_async(
    const SessionTitleStore& store,
    hermes::llm::LlmClient* aux,
    const std::string& model,
    const std::string& session_id,
    const std::string& user_message,
    const std::string& assistant_response,
    const std::vector<hermes::llm::Message>& conversation_history) {
    if (session_id.empty() || user_message.empty() ||
        assistant_response.empty()) {
        return;
    }
    if (!should_auto_title(conversation_history)) return;

    // Spawn a detached worker. Copies of the small arguments are fine;
    // aux is captured by pointer.
    std::thread worker(
        [store, aux, model, session_id, user_message, assistant_response]() {
            try {
                auto_title_session(store, aux, model, session_id,
                                   user_message, assistant_response);
            } catch (...) {
                // Swallow — detached background thread must not terminate.
            }
        });
    worker.detach();
}

}  // namespace hermes::agent
