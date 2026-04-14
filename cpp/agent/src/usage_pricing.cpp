// C++17 port of agent/usage_pricing.py pure math.
#include "hermes/agent/usage_pricing.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace hermes::agent::pricing {

namespace {

std::string lower_copy(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string last_slash_part(const std::string& s) {
    auto pos = s.rfind('/');
    if (pos == std::string::npos) return s;
    return s.substr(pos + 1);
}

std::optional<double> opt_from_json(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        try { return std::stod(it->get<std::string>()); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

}  // namespace

// ── CanonicalUsage ────────────────────────────────────────────────────

std::int64_t CanonicalUsage::prompt_tokens() const noexcept {
    return input_tokens + cache_read_tokens + cache_write_tokens;
}

std::int64_t CanonicalUsage::total_tokens() const noexcept {
    return prompt_tokens() + output_tokens;
}

// ── BillingRoute resolver ─────────────────────────────────────────────

BillingRoute resolve_billing_route(const std::string& model_in,
                                   const std::string& provider_in,
                                   const std::string& base_url) {
    std::string model = trim(model_in);
    std::string provider = lower_copy(trim(provider_in));
    std::string base = lower_copy(trim(base_url));

    // Infer provider from "anthropic/…", "openai/…", "google/…" prefix.
    if (provider.empty()) {
        auto slash = model.find('/');
        if (slash != std::string::npos) {
            std::string head = model.substr(0, slash);
            if (head == "anthropic" || head == "openai" || head == "google") {
                provider = head;
                model = model.substr(slash + 1);
            }
        }
    }

    BillingRoute r;
    r.base_url = base_url;
    if (provider == "openai-codex") {
        r.provider = "openai-codex";
        r.model = model;
        r.billing_mode = "subscription_included";
        return r;
    }
    if (provider == "openrouter" || base.find("openrouter.ai") != std::string::npos) {
        r.provider = "openrouter";
        r.model = model;
        r.billing_mode = "official_models_api";
        return r;
    }
    if (provider == "anthropic") {
        r.provider = "anthropic";
        r.model = last_slash_part(model);
        r.billing_mode = "official_docs_snapshot";
        return r;
    }
    if (provider == "openai") {
        r.provider = "openai";
        r.model = last_slash_part(model);
        r.billing_mode = "official_docs_snapshot";
        return r;
    }
    if (provider == "custom" || provider == "local" ||
        base.find("localhost") != std::string::npos) {
        r.provider = provider.empty() ? "custom" : provider;
        r.model = model;
        r.billing_mode = "unknown";
        return r;
    }
    r.provider = provider.empty() ? "unknown" : provider;
    r.model = model.empty() ? "" : last_slash_part(model);
    r.billing_mode = "unknown";
    return r;
}

// ── Cost math ─────────────────────────────────────────────────────────

bool has_known_pricing(const PricingEntry& e) {
    return e.input_cost_per_million.has_value() ||
           e.output_cost_per_million.has_value() ||
           e.cache_read_cost_per_million.has_value() ||
           e.cache_write_cost_per_million.has_value() ||
           e.request_cost.has_value();
}

CostResult estimate_usage_cost(const CanonicalUsage& usage,
                               const PricingEntry& entry) {
    CostResult r;
    r.source = entry.source;
    r.pricing_version = entry.pricing_version;

    if (!has_known_pricing(entry)) {
        r.status = status::kUnknown;
        r.label = "no pricing data";
        return r;
    }

    // If every known field is zero, flag as included.
    bool all_zero = true;
    auto check = [&](const std::optional<double>& v) {
        if (v.has_value() && *v != 0.0) all_zero = false;
    };
    check(entry.input_cost_per_million);
    check(entry.output_cost_per_million);
    check(entry.cache_read_cost_per_million);
    check(entry.cache_write_cost_per_million);
    check(entry.request_cost);

    if (all_zero) {
        r.amount_usd = 0.0;
        r.status = status::kIncluded;
        r.label = "included";
        return r;
    }

    double total = 0.0;
    auto add = [&](const std::optional<double>& per_million, std::int64_t tokens) {
        if (per_million.has_value() && tokens > 0) {
            total += (*per_million) * static_cast<double>(tokens) / 1000000.0;
        }
    };
    add(entry.input_cost_per_million, usage.input_tokens);
    add(entry.output_cost_per_million, usage.output_tokens);
    add(entry.cache_read_cost_per_million, usage.cache_read_tokens);
    add(entry.cache_write_cost_per_million, usage.cache_write_tokens);
    if (entry.request_cost.has_value()) {
        total += (*entry.request_cost) * static_cast<double>(usage.request_count);
    }

    r.amount_usd = total;
    r.status = status::kEstimated;
    r.label = std::to_string(total) + " USD (estimated)";
    return r;
}

// ── JSON helpers ──────────────────────────────────────────────────────

nlohmann::json cost_result_to_json(const CostResult& r) {
    nlohmann::json out = {
        {"status", r.status},
        {"source", r.source},
        {"label", r.label},
        {"pricing_version", r.pricing_version},
    };
    if (r.amount_usd.has_value()) out["amount_usd"] = *r.amount_usd;
    else out["amount_usd"] = nullptr;
    return out;
}

nlohmann::json pricing_entry_to_json(const PricingEntry& e) {
    nlohmann::json out = {
        {"source", e.source},
        {"source_url", e.source_url},
        {"pricing_version", e.pricing_version},
    };
    auto put = [&](const char* key, const std::optional<double>& v) {
        if (v.has_value()) out[key] = *v;
        else out[key] = nullptr;
    };
    put("input_cost_per_million", e.input_cost_per_million);
    put("output_cost_per_million", e.output_cost_per_million);
    put("cache_read_cost_per_million", e.cache_read_cost_per_million);
    put("cache_write_cost_per_million", e.cache_write_cost_per_million);
    put("request_cost", e.request_cost);
    return out;
}

PricingEntry pricing_entry_from_json(const nlohmann::json& j) {
    PricingEntry e;
    if (!j.is_object()) return e;
    e.input_cost_per_million = opt_from_json(j, "input_cost_per_million");
    e.output_cost_per_million = opt_from_json(j, "output_cost_per_million");
    e.cache_read_cost_per_million = opt_from_json(j, "cache_read_cost_per_million");
    e.cache_write_cost_per_million = opt_from_json(j, "cache_write_cost_per_million");
    e.request_cost = opt_from_json(j, "request_cost");
    if (j.contains("source") && j["source"].is_string())
        e.source = j["source"].get<std::string>();
    if (j.contains("source_url") && j["source_url"].is_string())
        e.source_url = j["source_url"].get<std::string>();
    if (j.contains("pricing_version") && j["pricing_version"].is_string())
        e.pricing_version = j["pricing_version"].get<std::string>();
    return e;
}

}  // namespace hermes::agent::pricing
