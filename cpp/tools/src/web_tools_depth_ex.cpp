// Implementation of hermes/tools/web_tools_depth_ex.hpp.
#include "hermes/tools/web_tools_depth_ex.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace hermes::tools::web::depth_ex {

namespace {

std::string trim_ascii(std::string_view s) {
    std::size_t b{0};
    std::size_t e{s.size()};
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::vector<std::string> split_dots(std::string_view s) {
    std::vector<std::string> out{};
    std::string acc{};
    for (char c : s) {
        if (c == '.') {
            out.push_back(acc);
            acc.clear();
        } else {
            acc.push_back(c);
        }
    }
    out.push_back(acc);
    return out;
}

}  // namespace

// ---- Query validation ---------------------------------------------------

std::string validate_query(std::string_view query) {
    std::string t{trim_ascii(query)};
    if (t.empty()) {
        return "Query must be non-empty.";
    }
    if (t.size() > kMaxQueryChars) {
        std::ostringstream os;
        os << "Query exceeds " << kMaxQueryChars << " characters.";
        return os.str();
    }
    return {};
}

bool is_http_url(std::string_view url) {
    std::string_view u{url};
    if (u.size() >= 7 && u.substr(0, 7) == "http://") {
        return u.size() > 7;
    }
    if (u.size() >= 8 && u.substr(0, 8) == "https://") {
        return u.size() > 8;
    }
    return false;
}

std::string validate_url_batch(const std::vector<std::string>& urls) {
    if (urls.empty()) {
        return "URL batch must contain at least one URL.";
    }
    if (urls.size() > kMaxBatchUrls) {
        std::ostringstream os;
        os << "URL batch exceeds " << kMaxBatchUrls << " entries.";
        return os.str();
    }
    for (const auto& u : urls) {
        if (!is_http_url(u)) {
            std::ostringstream os;
            os << "Invalid URL: '" << u << "' (must start with http:// or "
                                           "https://).";
            return os.str();
        }
    }
    return {};
}

// ---- Envelope unwrapping ------------------------------------------------

nlohmann::json unwrap_data(const nlohmann::json& response) {
    if (!response.is_object()) return response;
    auto it = response.find("data");
    if (it == response.end()) return response;
    if (it->is_object() || it->is_array()) return *it;
    return response;
}

nlohmann::json pluck_first_array(const nlohmann::json& response,
                                 const std::vector<std::string>& keys) {
    if (!response.is_object()) {
        return nlohmann::json::array();
    }
    for (const auto& key : keys) {
        const auto parts = split_dots(key);
        const nlohmann::json* cur{&response};
        bool ok{true};
        for (const auto& p : parts) {
            if (!cur->is_object()) { ok = false; break; }
            auto f = cur->find(p);
            if (f == cur->end()) { ok = false; break; }
            cur = &(*f);
        }
        if (ok && cur->is_array() && !cur->empty()) {
            return *cur;
        }
    }
    return nlohmann::json::array();
}

nlohmann::json to_plain_object(const nlohmann::json& value) {
    if (!value.is_object()) {
        return nlohmann::json{};
    }
    nlohmann::json out = nlohmann::json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.key().empty() && it.key().front() == '_') continue;
        out[it.key()] = it.value();
    }
    return out;
}

// ---- Document sanity checks --------------------------------------------

bool document_is_usable(const nlohmann::json& doc) {
    if (!doc.is_object()) return false;
    auto url_it = doc.find("url");
    if (url_it == doc.end() || !url_it->is_string()) return false;
    if (trim_ascii(url_it->get<std::string>()).empty()) return false;
    auto content_it = doc.find("content");
    if (content_it == doc.end() || !content_it->is_string()) return false;
    if (trim_ascii(content_it->get<std::string>()).empty()) return false;
    return true;
}

nlohmann::json strip_noise_fields(const nlohmann::json& doc) {
    if (!doc.is_object()) return doc;
    nlohmann::json out = doc;
    static const std::vector<std::string> kNoise{
        "raw_content", "html", "screenshot", "markdown", "links",
        "rawHtml",     "rawContent",
    };
    for (const auto& k : kNoise) {
        out.erase(k);
    }
    return out;
}

// ---- Parallel / Exa body assembly --------------------------------------

nlohmann::json build_parallel_search_body(std::string_view query,
                                          std::string_view search_mode,
                                          std::size_t max_results) {
    nlohmann::json body = nlohmann::json::object();
    body["query"] = std::string{query};
    body["search_mode"] = std::string{search_mode};
    constexpr std::size_t kMax{20u};
    body["max_results"] = std::min<std::size_t>(max_results, kMax);
    return body;
}

nlohmann::json build_exa_search_body(std::string_view query,
                                     std::size_t max_results,
                                     bool use_autoprompt) {
    nlohmann::json body = nlohmann::json::object();
    body["query"] = std::string{query};
    constexpr std::size_t kMax{20u};
    body["numResults"] = std::min<std::size_t>(max_results, kMax);
    body["useAutoprompt"] = use_autoprompt;
    return body;
}

// ---- Firecrawl error-shape detection -----------------------------------

std::string firecrawl_error_message(const nlohmann::json& response) {
    if (!response.is_object()) return {};
    // Firecrawl signals errors via a top-level "error" string or an
    // explicit success=false flag with a "message" / "detail" field.
    if (auto it = response.find("error");
        it != response.end() && it->is_string() &&
        !it->get<std::string>().empty()) {
        return it->get<std::string>();
    }
    if (auto s = response.find("success");
        s != response.end() && s->is_boolean() && !s->get<bool>()) {
        for (const auto* k : {"message", "detail", "reason"}) {
            if (auto it = response.find(k);
                it != response.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
        return "Unknown Firecrawl error (success=false).";
    }
    return {};
}

// ---- HTTP headers -------------------------------------------------------

std::optional<int> parse_retry_after_seconds(std::string_view header) {
    std::string t{trim_ascii(header)};
    if (t.empty()) return std::nullopt;
    int acc{0};
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        acc = acc * 10 + (c - '0');
        if (acc > 3'600) return 3'600;  // cap to 1h
    }
    return acc;
}

std::string default_user_agent() {
    return "Hermes-Agent/1.0 (+https://nousresearch.com/hermes)";
}

// ---- Summariser prompts ------------------------------------------------

std::string chunk_prompt_prefix(std::string_view title,
                                std::string_view source_url,
                                std::size_t chunk_index,
                                std::size_t total_chunks) {
    std::ostringstream os;
    if (!title.empty()) os << "Title: " << title << '\n';
    if (!source_url.empty()) os << "Source: " << source_url << '\n';
    os << "[Processing chunk " << (chunk_index + 1u) << " of "
       << total_chunks << "]\n\n";
    return os.str();
}

std::string single_shot_prompt_prefix(std::string_view title,
                                      std::string_view source_url) {
    std::ostringstream os;
    if (!title.empty()) os << "Title: " << title << '\n';
    if (!source_url.empty()) os << "Source: " << source_url << '\n';
    if (!title.empty() || !source_url.empty()) os << '\n';
    return os.str();
}

}  // namespace hermes::tools::web::depth_ex
