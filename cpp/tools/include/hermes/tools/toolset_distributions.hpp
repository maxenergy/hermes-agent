// Toolset distributions for synthetic trajectory generation.
//
// A distribution maps toolset names to inclusion probabilities (0..1).
// Sampling rolls each toolset independently and returns the included
// names.  When all rolls miss, the highest-probability toolset is
// guaranteed to be picked so the caller always gets at least one entry.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hermes::tools {

struct DistributionEntry {
    std::string toolset_name;
    double probability;  // 0.0 to 1.0
};

struct Distribution {
    std::string name;
    std::string description;
    std::vector<DistributionEntry> entries;
};

// All known distributions, keyed by name.  Mirrors DISTRIBUTIONS in
// toolset_distributions.py — probabilities are stored in [0, 1] here
// (Python uses 0..100, conversion happens at load time).
const std::map<std::string, Distribution>& distributions();

// Sample a subset of toolsets from a named distribution.
//
// When ``seed != 0`` the sampling uses a deterministic ``std::mt19937_64``
// seeded with ``seed`` so callers can reproduce a sample exactly.  When
// ``seed == 0`` a fresh ``std::random_device`` seed is used.
std::vector<std::string> sample_toolsets_from_distribution(
    const std::string& name, std::uint64_t seed = 0);

}  // namespace hermes::tools
