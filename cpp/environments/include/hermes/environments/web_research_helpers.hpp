// Depth port of environments/web_research_env.py pure helpers.
//
// WebResearchEnv is an Atropos RL environment where the model performs
// multi-hop web research and is graded on:
//   * answer correctness (LLM judge + heuristic fallback)
//   * whether web tools were actually used
//   * efficiency (number of tool calls)
//   * source diversity (distinct cited domains)
//
// Most of the environment logic is Atropos plumbing that does not port
// meaningfully to C++, but the scoring pieces are pure string/number
// transforms.  This header exposes those building blocks so we can
// assert byte-for-byte parity with the Python reward signal without
// running the environment.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::environments::web_research_helpers {

// --- Reward configuration (mirrors WebResearchEnvConfig) ---------------

struct RewardConfig {
    double correctness_weight{0.6};
    double tool_usage_weight{0.2};
    double efficiency_weight{0.2};
    double diversity_bonus{0.1};
    int efficient_max_calls{5};
    int heavy_penalty_calls{10};
};

// --- LLM judge parsing -------------------------------------------------

// Parse the LLM judge's JSON response and extract the ``score`` float.
// Accepts raw JSON as well as fenced code blocks ```` ```json ... ``` ````.
// If a JSON object fails to parse, the function falls back to a regex
// over the raw text.  Returns ``std::nullopt`` when no valid 0..1
// score can be recovered.
std::optional<double> parse_judge_json(std::string_view text);

// --- Heuristic score ---------------------------------------------------

// Keyword-overlap fallback scoring used when the LLM judge is
// unavailable.  Tokenizes both strings, removes stopwords and tokens
// <=2 characters, then combines Jaccard similarity (40%) and recall
// (60%) and returns the value in [0, 1].  When ``expected`` has no
// tokens the function returns ``0.5`` (ambiguous).
double heuristic_score(std::string_view expected, std::string_view model_answer);

// Default English stopword set used by ``heuristic_score``.  Exposed so
// callers can extend or override.
const std::unordered_set<std::string>& default_stopwords();

// Tokenize ``text`` into lowercase alphanumeric words, dropping
// stopwords and tokens of length <= 2 (matching the Python helper).
std::unordered_set<std::string>
tokenize(std::string_view text,
         const std::unordered_set<std::string>& stopwords = default_stopwords());

// --- URL / domain extraction ------------------------------------------

// Extract distinct HTTP/HTTPS domains cited anywhere in ``text``.
// Strips a single leading ``www.`` from the netloc.  Returns a set so
// duplicates across multiple citations collapse.
std::unordered_set<std::string> extract_domains(std::string_view text);

// Extract raw http(s) URLs from ``text``.  Matches ``https?://[^\s)>]\"'`]+``
// and deduplicates while preserving discovery order.
std::vector<std::string> extract_urls(std::string_view text);

// --- Efficiency / diversity / final reward ----------------------------

// Compute the efficiency signal for a given tool-call count.  Mirrors
// ``WebResearchEnv.compute_reward`` exactly:
//   * <= efficient_max_calls → 1.0
//   * in (efficient_max_calls, heavy_penalty_calls] → 1 - (n - emc) * 0.08
//   * > heavy_penalty_calls → max(0, 1 - (n - emc) * 0.12)
double efficiency_score(int tool_call_count, const RewardConfig& cfg);

// Check whether any of the caller-provided tool names belongs to the
// set of recognised web-research tools (``web_search``, ``web_extract``,
// ``search``, ``firecrawl``).
bool used_web_tool(const std::vector<std::string>& tools_used);

// Compute the diversity bonus given a set of cited domains.  Returns
// ``cfg.diversity_bonus`` when at least two distinct domains are
// present and zero otherwise.
double diversity_score(const std::unordered_set<std::string>& domains,
                        const RewardConfig& cfg);

// Combine all four signals into the final clamped reward in [0, 1].
double combine_reward(double correctness, double tool_used, double efficiency,
                       double diversity, const RewardConfig& cfg);

}  // namespace hermes::environments::web_research_helpers
