#include "hermes/tools/toolset_distributions.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace hermes::tools {

namespace {

Distribution make_dist(std::string name, std::string description,
                       std::vector<DistributionEntry> entries) {
    return Distribution{std::move(name), std::move(description),
                        std::move(entries)};
}

const std::map<std::string, Distribution>& distributions_storage() {
    static const std::map<std::string, Distribution> table = [] {
        std::map<std::string, Distribution> d;
        d["default"] = make_dist(
            "default", "All available tools, all the time",
            {{"web", 1.0}, {"vision", 1.0}, {"image_gen", 1.0},
             {"terminal", 1.0}, {"file", 1.0}, {"moa", 1.0}, {"browser", 1.0}});
        d["image_gen"] = make_dist(
            "image_gen", "Heavy focus on image generation",
            {{"image_gen", 0.90}, {"vision", 0.90}, {"web", 0.55},
             {"terminal", 0.45}, {"moa", 0.10}});
        d["research"] = make_dist(
            "research", "Web research with vision and reasoning",
            {{"web", 0.90}, {"browser", 0.70}, {"vision", 0.50},
             {"moa", 0.40}, {"terminal", 0.10}});
        d["science"] = make_dist(
            "science", "Scientific research with web/terminal/file/browser",
            {{"web", 0.94}, {"terminal", 0.94}, {"file", 0.94},
             {"vision", 0.65}, {"browser", 0.50}, {"image_gen", 0.15},
             {"moa", 0.10}});
        d["swe"] = make_dist(
            "swe", "Software engineering: terminal + file + reasoning",
            {{"terminal", 0.80}, {"file", 0.80}, {"moa", 0.60},
             {"web", 0.30}, {"vision", 0.10}});
        d["autonomous"] = make_dist(
            "autonomous", "Autonomous: high terminal/file/web availability",
            {{"terminal", 0.92}, {"file", 0.92}, {"web", 0.92},
             {"browser", 0.50}, {"vision", 0.15}, {"image_gen", 0.10}});
        return d;
    }();
    return table;
}

}  // namespace

const std::map<std::string, Distribution>& distributions() {
    return distributions_storage();
}

std::vector<std::string> sample_toolsets_from_distribution(
    const std::string& name, std::uint64_t seed) {
    const auto& table = distributions_storage();
    auto it = table.find(name);
    if (it == table.end()) {
        throw std::invalid_argument("unknown distribution: " + name);
    }
    const Distribution& dist = it->second;

    std::mt19937_64 rng;
    if (seed != 0) {
        rng.seed(seed);
    } else {
        std::random_device rd;
        rng.seed((static_cast<std::uint64_t>(rd()) << 32) | rd());
    }
    std::uniform_real_distribution<double> roll(0.0, 1.0);

    std::vector<std::string> out;
    for (const auto& entry : dist.entries) {
        if (roll(rng) < entry.probability) {
            out.push_back(entry.toolset_name);
        }
    }

    // Guarantee at least one toolset is selected — pick the highest
    // probability entry as the fallback (matches Python behavior).
    if (out.empty() && !dist.entries.empty()) {
        auto best = std::max_element(
            dist.entries.begin(), dist.entries.end(),
            [](const DistributionEntry& a, const DistributionEntry& b) {
                return a.probability < b.probability;
            });
        out.push_back(best->toolset_name);
    }

    return out;
}

}  // namespace hermes::tools
