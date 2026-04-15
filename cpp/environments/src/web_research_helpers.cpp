// Implementation of environments/web_research_env.py pure helpers.

#include <hermes/environments/web_research_helpers.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace hermes::environments::web_research_helpers {

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

std::string to_lower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string strip_fences(std::string_view s) {
    // Strip ``` and ```json markers.
    const std::regex fence_re{R"(```(?:json)?|```)"};
    return trim_copy(std::regex_replace(std::string{s}, fence_re, std::string{}));
}

}  // namespace

const std::unordered_set<std::string>& default_stopwords() {
    static const std::unordered_set<std::string> words{
        "the",   "a",    "an",  "is",   "are", "was", "were", "of",
        "in",    "on",   "at",  "to",   "for", "with", "and",  "or",
        "but",   "it",   "its", "this", "that","as",   "by",   "from",
        "be",    "has",  "have","had",
    };
    return words;
}

std::unordered_set<std::string>
tokenize(std::string_view text,
         const std::unordered_set<std::string>& stopwords) {
    std::unordered_set<std::string> out;
    const auto lower = to_lower(text);
    std::string current;
    current.reserve(32);
    for (auto c : lower) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            current.push_back(c);
        } else {
            if (current.size() > 2 &&
                stopwords.find(current) == stopwords.end()) {
                out.insert(current);
            }
            current.clear();
        }
    }
    if (current.size() > 2 && stopwords.find(current) == stopwords.end()) {
        out.insert(std::move(current));
    }
    return out;
}

std::optional<double> parse_judge_json(std::string_view text) {
    const auto cleaned = strip_fences(text);
    try {
        auto data = nlohmann::json::parse(cleaned);
        if (data.is_object() && data.contains("score")) {
            const auto& v = data.at("score");
            double score{-1.0};
            if (v.is_number()) {
                score = v.get<double>();
            } else if (v.is_string()) {
                try {
                    score = std::stod(v.get<std::string>());
                } catch (...) {
                    score = -1.0;
                }
            }
            if (score >= 0.0 && score <= 1.0) {
                return score;
            }
        }
    } catch (const nlohmann::json::exception&) {
        // fall through
    }
    // Regex fallback.
    const std::regex re{R"("score"\s*:\s*([0-9]+(?:\.[0-9]+)?))"};
    std::smatch m;
    const std::string s{text};
    if (std::regex_search(s, m, re)) {
        try {
            const auto score = std::stod(m.str(1));
            if (score >= 0.0 && score <= 1.0) {
                return score;
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

double heuristic_score(std::string_view expected, std::string_view model_answer) {
    const auto expected_tokens = tokenize(expected);
    const auto answer_tokens = tokenize(model_answer);
    if (expected_tokens.empty()) {
        return 0.5;
    }
    std::size_t overlap{0};
    std::size_t union_size = expected_tokens.size();
    for (const auto& t : answer_tokens) {
        if (expected_tokens.find(t) != expected_tokens.end()) {
            ++overlap;
        } else {
            ++union_size;
        }
    }
    const double jaccard = union_size > 0 ? static_cast<double>(overlap) /
                                                  static_cast<double>(union_size)
                                             : 0.0;
    const double recall = static_cast<double>(overlap) /
                            static_cast<double>(expected_tokens.size());
    const double combined = 0.4 * jaccard + 0.6 * recall;
    return combined > 1.0 ? 1.0 : combined;
}

std::vector<std::string> extract_urls(std::string_view text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const std::regex re{R"(https?://[^\s\)>\]\"']+)"};
    const std::string s{text};
    for (auto it = std::sregex_iterator{s.begin(), s.end(), re};
         it != std::sregex_iterator{}; ++it) {
        auto url = (*it).str();
        if (seen.insert(url).second) {
            out.push_back(std::move(url));
        }
    }
    return out;
}

std::unordered_set<std::string> extract_domains(std::string_view text) {
    std::unordered_set<std::string> domains;
    for (const auto& url : extract_urls(text)) {
        // Find "://" then netloc until first /, ?, #.
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            continue;
        }
        auto rest = url.substr(scheme_end + 3);
        const auto end = rest.find_first_of("/?#");
        if (end != std::string::npos) {
            rest = rest.substr(0, end);
        }
        auto lower = to_lower(rest);
        // Strip leading "www." (but not inside — Python uses lstrip("www.")
        // which is character-set strip, so match that.)
        // Python's str.lstrip("www.") strips any leading char in the set
        // {'w', '.'}, so "www.google.com" becomes "google.com" but also
        // "wwww.google.com" becomes "google.com". Emulate that.
        while (!lower.empty() && (lower.front() == 'w' || lower.front() == '.')) {
            lower.erase(lower.begin());
        }
        // Also strip embedded userinfo like user@host.
        const auto at = lower.rfind('@');
        if (at != std::string::npos) {
            lower = lower.substr(at + 1);
        }
        if (!lower.empty()) {
            domains.insert(std::move(lower));
        }
    }
    return domains;
}

double efficiency_score(int tool_call_count, const RewardConfig& cfg) {
    if (tool_call_count <= cfg.efficient_max_calls) {
        return 1.0;
    }
    if (tool_call_count <= cfg.heavy_penalty_calls) {
        return 1.0 -
               static_cast<double>(tool_call_count - cfg.efficient_max_calls) *
                   0.08;
    }
    const auto v = 1.0 -
                    static_cast<double>(tool_call_count - cfg.efficient_max_calls) *
                        0.12;
    return v < 0.0 ? 0.0 : v;
}

bool used_web_tool(const std::vector<std::string>& tools_used) {
    static const std::unordered_set<std::string> web{
        "web_search", "web_extract", "search", "firecrawl",
    };
    for (const auto& t : tools_used) {
        if (web.find(t) != web.end()) {
            return true;
        }
    }
    return false;
}

double diversity_score(const std::unordered_set<std::string>& domains,
                        const RewardConfig& cfg) {
    return domains.size() >= 2 ? cfg.diversity_bonus : 0.0;
}

double combine_reward(double correctness, double tool_used, double efficiency,
                       double diversity, const RewardConfig& cfg) {
    double reward = cfg.correctness_weight * correctness +
                     cfg.tool_usage_weight * tool_used +
                     cfg.efficiency_weight * efficiency + diversity;
    if (reward < 0.0) {
        reward = 0.0;
    }
    if (reward > 1.0) {
        reward = 1.0;
    }
    return reward;
}

}  // namespace hermes::environments::web_research_helpers
