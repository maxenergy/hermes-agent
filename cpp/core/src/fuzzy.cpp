#include "hermes/core/fuzzy.hpp"

#include <algorithm>
#include <vector>

namespace hermes::core::fuzzy {

int levenshtein(std::string_view a, std::string_view b) {
    const std::size_t m = a.size();
    const std::size_t n = b.size();
    if (m == 0) {
        return static_cast<int>(n);
    }
    if (n == 0) {
        return static_cast<int>(m);
    }

    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (std::size_t i = 0; i <= m; ++i) {
        dp[i][0] = static_cast<int>(i);
    }
    for (std::size_t j = 0; j <= n; ++j) {
        dp[0][j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i - 1][j] + 1,       // deletion
                dp[i][j - 1] + 1,       // insertion
                dp[i - 1][j - 1] + cost // substitution
            });
        }
    }
    return dp[m][n];
}

bool fuzzy_contains(std::string_view haystack, std::string_view needle, int max_distance) {
    if (needle.empty()) {
        return true;
    }
    if (max_distance < 0) {
        max_distance = 0;
    }
    if (needle.size() > haystack.size() + static_cast<std::size_t>(max_distance)) {
        return false;
    }
    // Slide a window across the haystack. We allow the window length to
    // vary within ±max_distance of |needle| to cover insertions and
    // deletions.
    const int needle_len = static_cast<int>(needle.size());
    const int hay_len = static_cast<int>(haystack.size());
    const int low = std::max(1, needle_len - max_distance);
    const int high = needle_len + max_distance;

    for (int start = 0; start <= hay_len; ++start) {
        for (int len = low; len <= high; ++len) {
            if (start + len > hay_len) {
                continue;
            }
            const auto window = haystack.substr(
                static_cast<std::size_t>(start), static_cast<std::size_t>(len));
            if (levenshtein(window, needle) <= max_distance) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace hermes::core::fuzzy
