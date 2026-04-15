// Implementation of environments/agentic_opd_env.py pure helpers.

#include <hermes/environments/opd_helpers.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace hermes::environments::opd_helpers {

namespace {

std::string trim_copy(std::string_view s) {
    auto begin = std::size_t{0};
    auto end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

}  // namespace

std::string default_hint_judge_system() {
    return std::string{
        "You are a process reward model used for hindsight hint extraction.\n"
        "You are given:\n"
        "1) The assistant response at turn t.\n"
        "2) The next state at turn t+1, along with its **role**.\n\n"
        "## Understanding the next state's role\n"
        "- role='user': A reply from the user (follow-up, correction, new "
        "request, etc.).\n"
        "- role='tool': The return value of a tool the assistant invoked. "
        "This content was NOT available before the assistant's action — it "
        "exists BECAUSE the assistant called the tool. A successful, "
        "non-error tool output generally means the assistant's action was "
        "appropriate; do NOT treat it as information the assistant should "
        "have already known.\n\n"
        "Your goal is to decide whether the next state reveals useful "
        "hindsight information\n"
        "that could have helped improve the assistant response at turn t.\n\n"
        "Output format rules (strict):\n"
        "- You MUST include exactly one final decision token: \\boxed{1} or "
        "\\boxed{-1}.\n"
        "- If and only if decision is \\boxed{1}, provide a concise, "
        "information-dense hint in 1-3 sentences,\n"
        "  wrapped between [HINT_START] and [HINT_END].\n"
        "- If decision is \\boxed{-1}, do not provide a hint block.\n"
        "- Hint must be concrete and actionable for improving the previous "
        "response."};
}

std::vector<nlohmann::json>
build_hint_judge_messages(std::string_view response_text,
                            std::string_view next_state_text,
                            std::string_view next_state_role,
                            std::string_view judge_system) {
    std::ostringstream user;
    user << "## Assistant response (turn t)\n"
         << response_text << "\n\n"
         << "## Next state (turn t+1) [role: " << next_state_role << "]\n"
         << next_state_text << "\n\n"
         << "Now output your decision and (if positive) the hint in the "
            "required format.";
    const auto system_text =
        judge_system.empty() ? default_hint_judge_system()
                              : std::string{judge_system};
    return {
        nlohmann::json{{"role", "system"}, {"content", system_text}},
        nlohmann::json{{"role", "user"}, {"content", user.str()}},
    };
}

HintResult parse_hint_result(std::string_view text) {
    HintResult result{};
    const std::string s{text};

    // Last \boxed{<int>} — but only +/- 1 is accepted.
    const std::regex boxed_re{R"(\\boxed\{(-?\d+)\})"};
    std::optional<int> last_score;
    for (auto it = std::sregex_iterator{s.begin(), s.end(), boxed_re};
         it != std::sregex_iterator{}; ++it) {
        try {
            last_score = std::stoi((*it).str(1));
        } catch (const std::exception&) {
            last_score.reset();
        }
    }
    if (last_score.has_value() && (*last_score == 1 || *last_score == -1)) {
        result.score = *last_score;
    }

    // Last [HINT_START]...[HINT_END] span.
    // std::regex has no DOTALL, so allow . to match newlines via [\s\S].
    const std::regex hint_re{R"(\[HINT_START\]([\s\S]*?)\[HINT_END\])"};
    std::string last_hint;
    for (auto it = std::sregex_iterator{s.begin(), s.end(), hint_re};
         it != std::sregex_iterator{}; ++it) {
        last_hint = (*it).str(1);
    }
    result.hint = trim_copy(last_hint);
    return result;
}

std::optional<HintResult>
select_best_hint(const std::vector<HintResult>& votes) {
    std::optional<HintResult> best;
    std::size_t best_len{0};
    for (const auto& v : votes) {
        if (!v.score.has_value() || *v.score != 1) {
            continue;
        }
        const auto trimmed = trim_copy(v.hint);
        if (trimmed.size() <= 10) {
            continue;
        }
        if (!best.has_value() || trimmed.size() > best_len) {
            best = v;
            best_len = trimmed.size();
        }
    }
    return best;
}

std::vector<nlohmann::json>
append_hint_to_messages(const std::vector<nlohmann::json>& messages,
                          std::string_view hint) {
    const auto cleaned_hint = trim_copy(hint);
    if (messages.empty()) {
        return {
            nlohmann::json{
                {"role", "user"},
                {"content",
                  std::string{"[user's hint / instruction]\n"} + cleaned_hint},
            },
        };
    }
    auto cloned = messages;
    // Find last user message
    auto target = static_cast<std::ptrdiff_t>(cloned.size()) - 1;
    for (; target >= 0; --target) {
        if (cloned[target].is_object() &&
            cloned[target].value("role", std::string{}) == "user") {
            break;
        }
    }
    if (target < 0) {
        target = static_cast<std::ptrdiff_t>(cloned.size()) - 1;
    }
    auto& msg = cloned[target];
    std::string content_text;
    if (msg.is_object() && msg.contains("content")) {
        const auto& c = msg.at("content");
        if (c.is_string()) {
            content_text = c.get<std::string>();
        } else if (c.is_array()) {
            std::ostringstream oss;
            bool first{true};
            for (const auto& part : c) {
                if (!first) {
                    oss << ' ';
                }
                first = false;
                if (part.is_object() && part.contains("text") &&
                    part.at("text").is_string()) {
                    oss << part.at("text").get<std::string>();
                } else {
                    oss << part.dump();
                }
            }
            content_text = oss.str();
        } else if (!c.is_null()) {
            content_text = c.dump();
        }
    }
    const std::string suffix =
        std::string{"\n\n[user's hint / instruction]\n"} + cleaned_hint;
    const auto merged = trim_copy(content_text + suffix);
    msg["content"] = merged;
    return cloned;
}

std::vector<TurnPair>
extract_turn_pairs(const std::vector<nlohmann::json>& messages,
                    std::size_t max_next_state_chars) {
    std::vector<TurnPair> pairs;
    std::size_t i{0};
    while (i < messages.size()) {
        const auto& msg = messages[i];
        const auto is_object = msg.is_object();
        const auto role = is_object ? msg.value("role", std::string{}) : std::string{};
        const auto content_is_string =
            is_object && msg.contains("content") && msg.at("content").is_string();
        const auto has_content =
            is_object && msg.contains("content") && !msg.at("content").is_null() &&
            (!content_is_string || !msg.at("content").get<std::string>().empty());
        if (role == "assistant" && has_content) {
            const auto assistant_text = content_is_string
                                               ? msg.at("content").get<std::string>()
                                               : msg.at("content").dump();
            std::vector<nlohmann::json> context(messages.begin(),
                                                  messages.begin() + static_cast<std::ptrdiff_t>(i));
            std::vector<nlohmann::json> next_states;
            std::size_t j{i + 1};
            while (j < messages.size()) {
                const auto& nm = messages[j];
                const auto nrole = nm.is_object() ? nm.value("role", std::string{}) : std::string{};
                if (nrole == "tool") {
                    next_states.push_back(nm);
                    ++j;
                } else if (nrole == "user") {
                    next_states.push_back(nm);
                    break;
                } else {
                    break;
                }
            }
            if (!next_states.empty()) {
                std::vector<std::string> parts;
                std::string next_role =
                    next_states.front().is_object()
                        ? next_states.front().value("role", std::string{"tool"})
                        : std::string{"tool"};
                for (const auto& ns : next_states) {
                    if (!ns.is_object() || !ns.contains("content")) {
                        continue;
                    }
                    const auto& c = ns.at("content");
                    std::string text =
                        c.is_string() ? c.get<std::string>() : c.dump();
                    if (text.empty()) {
                        continue;
                    }
                    if (text.size() > max_next_state_chars) {
                        text = text.substr(0, max_next_state_chars) +
                                 "\n...[truncated]";
                    }
                    parts.push_back(std::move(text));
                }
                if (!parts.empty()) {
                    std::ostringstream oss;
                    for (std::size_t k{0}; k < parts.size(); ++k) {
                        if (k > 0) {
                            oss << "\n---\n";
                        }
                        oss << parts[k];
                    }
                    auto next_text = oss.str();
                    // Strip-whitespace check equivalent.
                    if (!trim_copy(next_text).empty()) {
                        TurnPair pair;
                        pair.context_messages = std::move(context);
                        pair.assistant_text = assistant_text;
                        pair.next_state_text = std::move(next_text);
                        pair.next_state_role = std::move(next_role);
                        pairs.push_back(std::move(pair));
                    }
                }
            }
        }
        ++i;
    }
    return pairs;
}

std::optional<std::size_t>
find_token_span(const std::vector<int>& full, const std::vector<int>& sub) {
    if (sub.empty() || full.empty() || sub.size() > full.size()) {
        return std::nullopt;
    }
    const auto sub_len = sub.size();
    const auto full_len = full.size();
    // Search from the end for efficiency (Python does the same).
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(full_len - sub_len);
         i >= 0; --i) {
        bool match{true};
        for (std::size_t k{0}; k < sub_len; ++k) {
            if (full[static_cast<std::size_t>(i) + k] != sub[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            return static_cast<std::size_t>(i);
        }
    }
    return std::nullopt;
}

}  // namespace hermes::environments::opd_helpers
