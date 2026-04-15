// Implementation of hermes/tools/web_tools_depth.hpp.
#include "hermes/tools/web_tools_depth.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <regex>
#include <sstream>
#include <string>

namespace hermes::tools::web {

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

std::string trim_ascii(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return std::string{s.substr(b, e - b)};
}

std::string json_str(const nlohmann::json& v, std::string_view fallback = "") {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return std::string{fallback};
}

nlohmann::json build_document(std::string_view url,
                              std::string_view title,
                              std::string_view content,
                              std::string_view raw_content,
                              std::optional<std::string> err = std::nullopt) {
    nlohmann::json doc;
    doc["url"] = std::string{url};
    doc["title"] = std::string{title};
    doc["content"] = std::string{content};
    doc["raw_content"] = std::string{raw_content};
    if (err.has_value()) {
        doc["error"] = *err;
    }
    nlohmann::json meta;
    meta["sourceURL"] = std::string{url};
    meta["title"] = std::string{title};
    doc["metadata"] = std::move(meta);
    return doc;
}

}  // namespace

// ---- Backend selection --------------------------------------------------

WebBackend parse_web_backend(std::string_view name) {
    const std::string lower = to_lower(trim_ascii(name));
    if (lower == "firecrawl") {
        return WebBackend::Firecrawl;
    }
    if (lower == "parallel") {
        return WebBackend::Parallel;
    }
    if (lower == "tavily") {
        return WebBackend::Tavily;
    }
    if (lower == "exa") {
        return WebBackend::Exa;
    }
    return WebBackend::Unknown;
}

std::string web_backend_name(WebBackend backend) {
    switch (backend) {
        case WebBackend::Firecrawl:
            return "firecrawl";
        case WebBackend::Parallel:
            return "parallel";
        case WebBackend::Tavily:
            return "tavily";
        case WebBackend::Exa:
            return "exa";
        case WebBackend::Unknown:
        default:
            return "unknown";
    }
}

bool is_backend_available(WebBackend backend, const BackendAvailability& avail) {
    switch (backend) {
        case WebBackend::Exa:
            return avail.exa_key;
        case WebBackend::Parallel:
            return avail.parallel_key;
        case WebBackend::Firecrawl:
            return avail.firecrawl_key || avail.firecrawl_url ||
                   avail.gateway_ready;
        case WebBackend::Tavily:
            return avail.tavily_key;
        case WebBackend::Unknown:
        default:
            return false;
    }
}

WebBackend resolve_backend(std::string_view configured,
                           const BackendAvailability& avail) {
    const WebBackend explicit_choice = parse_web_backend(configured);
    if (explicit_choice != WebBackend::Unknown) {
        return explicit_choice;
    }
    // Fallback order matches Python: firecrawl > parallel > tavily > exa.
    if (is_backend_available(WebBackend::Firecrawl, avail)) {
        return WebBackend::Firecrawl;
    }
    if (is_backend_available(WebBackend::Parallel, avail)) {
        return WebBackend::Parallel;
    }
    if (is_backend_available(WebBackend::Tavily, avail)) {
        return WebBackend::Tavily;
    }
    if (is_backend_available(WebBackend::Exa, avail)) {
        return WebBackend::Exa;
    }
    return WebBackend::Firecrawl;
}

// ---- Tavily helpers -----------------------------------------------------

nlohmann::json normalise_tavily_search_results(const nlohmann::json& response) {
    nlohmann::json web = nlohmann::json::array();
    if (response.is_object() && response.contains("results") &&
        response.at("results").is_array()) {
        std::size_t idx = 0;
        for (const auto& r : response.at("results")) {
            if (!r.is_object()) {
                ++idx;
                continue;
            }
            nlohmann::json row;
            row["title"] = json_str(r.value("title", nlohmann::json{}));
            row["url"] = json_str(r.value("url", nlohmann::json{}));
            row["description"] = json_str(r.value("content", nlohmann::json{}));
            row["position"] = static_cast<int>(idx + 1);
            web.push_back(std::move(row));
            ++idx;
        }
    }
    nlohmann::json envelope;
    envelope["success"] = true;
    envelope["data"]["web"] = std::move(web);
    return envelope;
}

nlohmann::json normalise_tavily_documents(const nlohmann::json& response,
                                          std::string_view fallback_url) {
    nlohmann::json docs = nlohmann::json::array();
    if (!response.is_object()) {
        return docs;
    }

    if (response.contains("results") && response.at("results").is_array()) {
        for (const auto& r : response.at("results")) {
            if (!r.is_object()) {
                continue;
            }
            const std::string url =
                json_str(r.value("url", nlohmann::json{}), fallback_url);
            std::string raw = json_str(r.value("raw_content", nlohmann::json{}));
            if (raw.empty()) {
                raw = json_str(r.value("content", nlohmann::json{}));
            }
            const std::string title = json_str(r.value("title", nlohmann::json{}));
            docs.push_back(build_document(url, title, raw, raw));
        }
    }

    if (response.contains("failed_results") &&
        response.at("failed_results").is_array()) {
        for (const auto& f : response.at("failed_results")) {
            if (!f.is_object()) {
                continue;
            }
            const std::string url =
                json_str(f.value("url", nlohmann::json{}), fallback_url);
            const std::string err =
                json_str(f.value("error", nlohmann::json{}), "extraction failed");
            docs.push_back(build_document(url, "", "", "", err));
        }
    }

    if (response.contains("failed_urls") &&
        response.at("failed_urls").is_array()) {
        for (const auto& fu : response.at("failed_urls")) {
            std::string url_str;
            if (fu.is_string()) {
                url_str = fu.get<std::string>();
            } else {
                url_str = fu.dump();
            }
            docs.push_back(
                build_document(url_str, "", "", "", "extraction failed"));
        }
    }
    return docs;
}

// ---- Result extraction --------------------------------------------------

nlohmann::json normalise_result_list(const nlohmann::json& values) {
    nlohmann::json out = nlohmann::json::array();
    if (!values.is_array()) {
        return out;
    }
    for (const auto& item : values) {
        if (item.is_object()) {
            out.push_back(item);
        }
    }
    return out;
}

nlohmann::json extract_web_search_results(const nlohmann::json& response) {
    if (response.is_object()) {
        if (response.contains("data")) {
            const auto& data = response.at("data");
            if (data.is_array()) {
                return normalise_result_list(data);
            }
            if (data.is_object()) {
                if (data.contains("web")) {
                    nlohmann::json web = normalise_result_list(data.at("web"));
                    if (!web.empty()) {
                        return web;
                    }
                }
                if (data.contains("results")) {
                    nlohmann::json res =
                        normalise_result_list(data.at("results"));
                    if (!res.empty()) {
                        return res;
                    }
                }
            }
        }
        if (response.contains("web")) {
            nlohmann::json web = normalise_result_list(response.at("web"));
            if (!web.empty()) {
                return web;
            }
        }
        if (response.contains("results")) {
            nlohmann::json res =
                normalise_result_list(response.at("results"));
            if (!res.empty()) {
                return res;
            }
        }
    }
    return nlohmann::json::array();
}

nlohmann::json extract_scrape_payload(const nlohmann::json& response) {
    if (!response.is_object()) {
        return nlohmann::json::object();
    }
    if (response.contains("data") && response.at("data").is_object()) {
        return response.at("data");
    }
    return response;
}

// ---- Content preprocessing ----------------------------------------------

std::string clean_base64_images(std::string_view text) {
    // Two passes matching the Python regexes exactly.
    const std::regex parens{R"(\(data:image/[^;]+;base64,[A-Za-z0-9+/=]+\))"};
    const std::regex bare{R"(data:image/[^;]+;base64,[A-Za-z0-9+/=]+)"};
    std::string intermediate =
        std::regex_replace(std::string{text}, parens, "[BASE64_IMAGE_REMOVED]");
    return std::regex_replace(intermediate, bare, "[BASE64_IMAGE_REMOVED]");
}

std::string build_summariser_context(std::string_view title,
                                     std::string_view url) {
    std::string ctx;
    if (!title.empty()) {
        ctx += "Title: ";
        ctx += std::string{title};
    }
    if (!url.empty()) {
        if (!ctx.empty()) {
            ctx += '\n';
        }
        ctx += "Source: ";
        ctx += std::string{url};
    }
    if (ctx.empty()) {
        return ctx;
    }
    ctx += "\n\n";
    return ctx;
}

// ---- Chunking -----------------------------------------------------------

SummariserDecision decide_summariser_mode(std::size_t content_len,
                                          std::size_t min_length) {
    if (content_len > kWebSummariserMaxContent) {
        return SummariserDecision::Refuse;
    }
    if (content_len < min_length) {
        return SummariserDecision::SkipTooShort;
    }
    if (content_len > kWebSummariserChunkThreshold) {
        return SummariserDecision::Chunked;
    }
    return SummariserDecision::SingleShot;
}

std::vector<std::string> split_content_into_chunks(std::string_view content,
                                                   std::size_t chunk_size) {
    std::vector<std::string> chunks;
    if (chunk_size == 0) {
        chunks.emplace_back(content);
        return chunks;
    }
    const std::size_t total = content.size();
    for (std::size_t i = 0; i < total; i += chunk_size) {
        const std::size_t n = std::min(chunk_size, total - i);
        chunks.emplace_back(content.substr(i, n));
    }
    return chunks;
}

std::string format_chunk_info(std::size_t chunk_index_zero_based,
                              std::size_t total_chunks) {
    std::ostringstream oss;
    oss << "[Processing chunk " << (chunk_index_zero_based + 1) << " of "
        << total_chunks << "]";
    return oss.str();
}

std::string cap_summary_output(std::string_view text, std::size_t max_output) {
    if (text.size() <= max_output) {
        return std::string{text};
    }
    std::string out{text.substr(0, max_output)};
    out += "\n\n[... summary truncated for context management ...]";
    return out;
}

std::string format_too_large_message(std::size_t size_bytes) {
    const double mb = static_cast<double>(size_bytes) / 1'000'000.0;
    char mb_buf[32];
    std::snprintf(mb_buf, sizeof(mb_buf), "%.1f", mb);
    std::string out = "[Content too large to process: ";
    out += mb_buf;
    out +=
        "MB. Try using web_crawl with specific extraction instructions, or "
        "search for a more focused source.]";
    return out;
}

std::string format_truncation_footer(std::size_t max_output,
                                     std::size_t total_len) {
    std::ostringstream oss;
    oss << "\n\n[Content truncated — showing first " << max_output << " of "
        << total_len
        << " chars. LLM summarization timed out. "
        << "To fix: increase auxiliary.web_extract.timeout in config.yaml, "
        << "or use a faster auxiliary model. Use browser_navigate for the "
           "full page.]";
    return oss.str();
}

// ---- Environment introspection -----------------------------------------

std::vector<std::string> web_required_env_vars(bool managed_nous) {
    std::vector<std::string> vars = {
        "EXA_API_KEY",       "PARALLEL_API_KEY", "TAVILY_API_KEY",
        "FIRECRAWL_API_KEY", "FIRECRAWL_API_URL",
    };
    if (managed_nous) {
        vars.emplace_back("FIRECRAWL_GATEWAY_URL");
        vars.emplace_back("TOOL_GATEWAY_DOMAIN");
        vars.emplace_back("TOOL_GATEWAY_SCHEME");
        vars.emplace_back("TOOL_GATEWAY_USER_TOKEN");
    }
    return vars;
}

std::string url_hostname_lower(std::string_view url) {
    // Very small URL parser — just enough for hostname extraction.
    auto scheme_end = url.find("://");
    std::string_view rest =
        (scheme_end == std::string_view::npos) ? url : url.substr(scheme_end + 3);
    // Trim path / query / fragment.
    const auto path_start = rest.find_first_of("/?#");
    if (path_start != std::string_view::npos) {
        rest = rest.substr(0, path_start);
    }
    // Drop user-info.
    const auto at = rest.find('@');
    if (at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }
    // Handle bracketed IPv6 hosts.
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        if (close != std::string_view::npos) {
            return to_lower(rest.substr(1, close - 1));
        }
    }
    // Drop :port.
    const auto colon = rest.find(':');
    if (colon != std::string_view::npos) {
        rest = rest.substr(0, colon);
    }
    return to_lower(rest);
}

bool is_nous_auxiliary_base_url(std::string_view url) {
    const std::string host = url_hostname_lower(url);
    if (host.empty()) {
        return false;
    }
    if (host == "nousresearch.com") {
        return true;
    }
    const std::string suffix = ".nousresearch.com";
    return host.size() > suffix.size() &&
           host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- Parallel search mode ----------------------------------------------

std::string parse_parallel_search_mode(std::string_view raw) {
    const std::string lower = to_lower(trim_ascii(raw));
    if (lower == "fast" || lower == "one-shot" || lower == "agentic") {
        return lower;
    }
    return "agentic";
}

}  // namespace hermes::tools::web
