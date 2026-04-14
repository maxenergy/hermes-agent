#include "hermes/agent/credential_pool.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <string>
#include <vector>

namespace hermes::agent::creds {

std::string strategy_name(Strategy s) {
    switch (s) {
        case Strategy::FillFirst: return "fill_first";
        case Strategy::RoundRobin: return "round_robin";
        case Strategy::Random: return "random";
        case Strategy::LeastUsed: return "least_used";
    }
    return "fill_first";
}

std::optional<Strategy> parse_strategy(const std::string& name) {
    std::string lower(name.size(), '\0');
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "fill_first") return Strategy::FillFirst;
    if (lower == "round_robin") return Strategy::RoundRobin;
    if (lower == "random") return Strategy::Random;
    if (lower == "least_used") return Strategy::LeastUsed;
    return std::nullopt;
}

std::vector<PoolEntry> filter_available(const std::vector<PoolEntry>& entries,
                                        std::int64_t now_sec) {
    std::vector<PoolEntry> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
        if (e.status == status::kExhausted && e.exhausted_until_sec > now_sec) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

std::optional<PoolEntry> choose_next(const std::vector<PoolEntry>& entries,
                                     Strategy strategy,
                                     std::int64_t now_sec,
                                     std::uint64_t round_robin_cursor,
                                     std::uint64_t random_seed) {
    auto available = filter_available(entries, now_sec);
    if (available.empty()) return std::nullopt;

    switch (strategy) {
        case Strategy::FillFirst:
            return available.front();
        case Strategy::RoundRobin: {
            std::size_t idx = round_robin_cursor % available.size();
            return available[idx];
        }
        case Strategy::Random: {
            std::mt19937_64 rng(random_seed == 0
                                    ? static_cast<std::uint64_t>(now_sec)
                                    : random_seed);
            std::uniform_int_distribution<std::size_t> dist(0, available.size() - 1);
            return available[dist(rng)];
        }
        case Strategy::LeastUsed: {
            auto it = std::min_element(available.begin(), available.end(),
                                       [](const PoolEntry& a, const PoolEntry& b) {
                                           if (a.use_count != b.use_count) {
                                               return a.use_count < b.use_count;
                                           }
                                           return a.last_used_sec < b.last_used_sec;
                                       });
            return *it;
        }
    }
    return available.front();
}

PoolEntry mark_exhausted(PoolEntry entry,
                         std::int64_t now_sec,
                         std::int64_t until_sec) {
    entry.status = status::kExhausted;
    if (until_sec <= 0) {
        entry.exhausted_until_sec = now_sec + kExhaustedTtlDefaultSeconds;
    } else {
        entry.exhausted_until_sec = until_sec;
    }
    return entry;
}

std::string pool_key(const std::string& provider,
                     const std::string& custom_name) {
    if (!custom_name.empty()) return std::string(kCustomPoolPrefix) + custom_name;
    return provider;
}

}  // namespace hermes::agent::creds
