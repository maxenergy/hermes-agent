#include "hermes/agent/title_generator.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace hermes::agent {

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front()) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

std::string generate_title(hermes::llm::LlmClient* aux,
                           std::string_view model,
                           std::string_view first_user_message) {
    if (!aux || model.empty() || first_user_message.empty()) return {};

    using hermes::llm::CompletionRequest;
    using hermes::llm::Message;
    using hermes::llm::Role;

    Message system_msg;
    system_msg.role = Role::System;
    system_msg.content_text =
        "Generate a short title (no more than 8 words) summarising the "
        "user's request below.  Return ONLY the title text — no quotes, "
        "no prefixes, no trailing punctuation.";

    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content_text = std::string(first_user_message.substr(
        0, std::min<size_t>(first_user_message.size(), 500)));

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
        title = strip_quotes(trim(title));
        // Drop a leading "Title:" prefix if the model added one.
        const std::string prefix = "title:";
        if (title.size() >= prefix.size()) {
            std::string lower = title.substr(0, prefix.size());
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower == prefix) {
                title = trim(title.substr(prefix.size()));
            }
        }
        if (title.size() > 60) {
            title = title.substr(0, 57) + "...";
        }
        return title;
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

}  // namespace hermes::agent
