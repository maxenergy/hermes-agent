// Depth port of agent/context_compressor.py pure helpers.  See the
// header for the intended Python→C++ mapping.

#include "hermes/agent/context_compressor_depth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hermes::agent::compressor_depth {

namespace {

std::string to_upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Python str[:head] + "\n...[truncated]...\n" + str[-tail:]
std::string truncate_middle(std::string_view content,
                            std::size_t head_len,
                            std::size_t tail_len) {
    if (content.size() <= head_len + tail_len) return std::string(content);
    std::string out;
    out.reserve(head_len + tail_len + 32);
    out.append(content.substr(0, head_len));
    out.append("\n...[truncated]...\n");
    out.append(content.substr(content.size() - tail_len));
    return out;
}

std::string lstrip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return std::string(s.substr(i));
}

std::string strip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return std::string(s.substr(i, j - i));
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

}  // namespace

std::int64_t estimate_message_tokens_rough(const Message& msg) {
    std::int64_t tokens = static_cast<std::int64_t>(msg.content.size()) / kCharsPerToken + 10;
    for (const auto& tc : msg.tool_calls) {
        tokens += static_cast<std::int64_t>(tc.arguments.size()) / kCharsPerToken;
    }
    return tokens;
}

std::int64_t estimate_messages_tokens_rough(const std::vector<Message>& msgs) {
    std::int64_t sum = 0;
    for (const auto& m : msgs) sum += estimate_message_tokens_rough(m);
    return sum;
}

std::int64_t derive_max_summary_tokens(std::int64_t context_length) {
    std::int64_t scaled = static_cast<std::int64_t>(
        std::floor(static_cast<double>(context_length) * 0.05));
    return std::min<std::int64_t>(scaled, kSummaryTokensCeiling);
}

std::int64_t derive_tail_token_budget(std::int64_t threshold_tokens,
                                      double summary_target_ratio) {
    if (threshold_tokens <= 0) return 0;
    double ratio = clamp_summary_target_ratio(summary_target_ratio);
    return static_cast<std::int64_t>(
        std::floor(static_cast<double>(threshold_tokens) * ratio));
}

std::int64_t derive_threshold_tokens(std::int64_t context_length,
                                     double threshold_percent) {
    if (context_length <= 0 || threshold_percent <= 0.0) return 0;
    return static_cast<std::int64_t>(
        std::floor(static_cast<double>(context_length) * threshold_percent));
}

double clamp_summary_target_ratio(double ratio) {
    return std::max(0.10, std::min(ratio, 0.80));
}

std::int64_t compute_summary_budget(const std::vector<Message>& turns,
                                    const SummaryBudgetConfig& cfg) {
    std::int64_t content_tokens = estimate_messages_tokens_rough(turns);
    std::int64_t budget = static_cast<std::int64_t>(
        std::floor(static_cast<double>(content_tokens) * kSummaryRatio));
    std::int64_t upper = std::min<std::int64_t>(budget, cfg.max_summary_tokens);
    return std::max<std::int64_t>(kMinSummaryTokens, upper);
}

std::string with_summary_prefix(std::string_view summary) {
    std::string text = strip(summary);
    if (!text.empty()) {
        for (auto prefix : {kLegacySummaryPrefix, kSummaryPrefix}) {
            if (starts_with(text, prefix)) {
                text = lstrip(text.substr(prefix.size()));
                break;
            }
        }
    }
    if (text.empty()) return std::string(kSummaryPrefix);
    std::string out(kSummaryPrefix);
    out.push_back('\n');
    out.append(text);
    return out;
}

std::vector<Message> sanitize_tool_pairs(std::vector<Message> messages,
                                         SanitizeReport* report) {
    SanitizeReport rep;
    std::unordered_set<std::string> surviving_call_ids;
    for (const auto& m : messages) {
        if (m.role != "assistant") continue;
        for (const auto& tc : m.tool_calls) {
            if (!tc.id.empty()) surviving_call_ids.insert(tc.id);
        }
    }
    std::unordered_set<std::string> result_call_ids;
    for (const auto& m : messages) {
        if (m.role == "tool" && !m.tool_call_id.empty()) {
            result_call_ids.insert(m.tool_call_id);
        }
    }

    // Orphan results: result_call_ids - surviving_call_ids
    std::unordered_set<std::string> orphaned;
    for (const auto& cid : result_call_ids) {
        if (surviving_call_ids.count(cid) == 0) orphaned.insert(cid);
    }
    if (!orphaned.empty()) {
        std::vector<Message> filtered;
        filtered.reserve(messages.size());
        for (auto& m : messages) {
            if (m.role == "tool" && orphaned.count(m.tool_call_id) != 0) {
                continue;
            }
            filtered.push_back(std::move(m));
        }
        messages = std::move(filtered);
        rep.orphan_results_removed = orphaned.size();
    }

    // Missing results: surviving_call_ids - result_call_ids
    std::unordered_set<std::string> missing;
    for (const auto& cid : surviving_call_ids) {
        if (result_call_ids.count(cid) == 0) missing.insert(cid);
    }
    if (!missing.empty()) {
        std::vector<Message> patched;
        patched.reserve(messages.size() + missing.size());
        for (auto& m : messages) {
            patched.push_back(std::move(m));
            if (patched.back().role == "assistant") {
                for (const auto& tc : patched.back().tool_calls) {
                    if (missing.count(tc.id) != 0) {
                        Message stub;
                        stub.role = "tool";
                        stub.content =
                            "[Result from earlier conversation — see context summary above]";
                        stub.tool_call_id = tc.id;
                        patched.push_back(std::move(stub));
                    }
                }
            }
        }
        messages = std::move(patched);
        rep.stubs_inserted = missing.size();
    }

    if (report) *report = rep;
    return messages;
}

std::size_t align_boundary_forward(const std::vector<Message>& messages,
                                   std::size_t idx) {
    while (idx < messages.size() && messages[idx].role == "tool") {
        ++idx;
    }
    return idx;
}

std::size_t align_boundary_backward(const std::vector<Message>& messages,
                                    std::size_t idx) {
    if (idx == 0 || idx >= messages.size()) return idx;
    // Walk backward past consecutive tool results.
    std::size_t check = idx - 1;
    bool passed_tool = false;
    while (check < messages.size() && messages[check].role == "tool") {
        passed_tool = true;
        if (check == 0) { check = static_cast<std::size_t>(-1); break; }
        --check;
    }
    (void)passed_tool;
    if (check < messages.size()
        && messages[check].role == "assistant"
        && !messages[check].tool_calls.empty()) {
        idx = check;
    }
    return idx;
}

std::size_t find_tail_cut_by_tokens(const std::vector<Message>& messages,
                                    std::size_t head_end,
                                    std::int64_t token_budget) {
    std::size_t n = messages.size();
    if (n <= head_end) return n;
    std::size_t min_tail = 0;
    if (n - head_end > 1) {
        min_tail = std::min<std::size_t>(3u, n - head_end - 1);
    }
    std::int64_t soft_ceiling = static_cast<std::int64_t>(
        std::floor(static_cast<double>(token_budget) * 1.5));
    std::int64_t accumulated = 0;
    std::size_t cut_idx = n;  // start from beyond the end

    // Iterate from n-1 down to head_end (inclusive).
    // Python: for i in range(n - 1, head_end - 1, -1)
    // which includes head_end when it is valid (>=0).
    std::size_t start = n == 0 ? 0 : n - 1;
    std::size_t stop_exclusive = head_end;  // iterate while i >= head_end
    for (std::size_t i = start; i + 1 > stop_exclusive && i < n;) {
        const auto& m = messages[i];
        std::int64_t msg_tokens = estimate_message_tokens_rough(m);
        if (accumulated + msg_tokens > soft_ceiling
            && (n - i) >= min_tail) {
            break;
        }
        accumulated += msg_tokens;
        cut_idx = i;
        if (i == 0) break;
        --i;
        if (i + 1 == stop_exclusive) break;
    }

    std::size_t fallback_cut = n - min_tail;
    if (cut_idx > fallback_cut) cut_idx = fallback_cut;
    if (cut_idx <= head_end) {
        cut_idx = std::max<std::size_t>(fallback_cut, head_end + 1);
    }
    cut_idx = align_boundary_backward(messages, cut_idx);
    return std::max<std::size_t>(cut_idx, head_end + 1);
}

std::vector<Message> prune_old_tool_results(std::vector<Message> messages,
                                            std::size_t protect_tail_count,
                                            std::int64_t protect_tail_tokens,
                                            PruneReport* report) {
    if (messages.empty()) {
        if (report) report->pruned = 0;
        return messages;
    }
    std::size_t n = messages.size();
    std::size_t prune_boundary = 0;
    if (protect_tail_tokens > 0) {
        std::int64_t accumulated = 0;
        std::size_t boundary = n;
        std::size_t min_protect = protect_tail_count < n
            ? std::min(protect_tail_count, n - 1) : (n - 1);
        for (std::size_t i = n - 1;; --i) {
            std::int64_t msg_tokens = estimate_message_tokens_rough(messages[i]);
            if (accumulated + msg_tokens > protect_tail_tokens
                && (n - i) >= min_protect) {
                boundary = i;
                break;
            }
            accumulated += msg_tokens;
            boundary = i;
            if (i == 0) break;
        }
        std::size_t floor_cut = n > min_protect ? n - min_protect : 0;
        prune_boundary = std::max<std::size_t>(boundary, floor_cut);
    } else {
        prune_boundary = n > protect_tail_count ? n - protect_tail_count : 0;
    }

    std::size_t pruned = 0;
    for (std::size_t i = 0; i < prune_boundary; ++i) {
        auto& m = messages[i];
        if (m.role != "tool") continue;
        if (m.content.empty()) continue;
        if (m.content == std::string(kPrunedToolPlaceholder)) continue;
        if (m.content.size() > 200) {
            m.content = std::string(kPrunedToolPlaceholder);
            ++pruned;
        }
    }
    if (report) report->pruned = pruned;
    return messages;
}

std::string serialize_for_summary(const std::vector<Message>& turns,
                                  const SerializeConfig& cfg) {
    std::vector<std::string> parts;
    parts.reserve(turns.size());
    for (const auto& msg : turns) {
        std::string role = msg.role.empty() ? "unknown" : msg.role;
        std::string content = msg.content;

        if (role == "tool") {
            if (content.size() > cfg.content_max) {
                content = truncate_middle(content, cfg.content_head, cfg.content_tail);
            }
            std::string tool_id = msg.tool_call_id;
            parts.push_back("[TOOL RESULT " + tool_id + "]: " + content);
            continue;
        }

        if (role == "assistant") {
            if (content.size() > cfg.content_max) {
                content = truncate_middle(content, cfg.content_head, cfg.content_tail);
            }
            if (!msg.tool_calls.empty()) {
                std::ostringstream oss;
                oss << content << "\n[Tool calls:\n";
                bool first = true;
                for (const auto& tc : msg.tool_calls) {
                    if (!first) oss << "\n";
                    first = false;
                    std::string name = tc.name.empty() ? "?" : tc.name;
                    std::string args = tc.arguments;
                    if (args.size() > cfg.tool_args_max) {
                        args = args.substr(0, cfg.tool_args_head) + "...";
                    }
                    oss << "  " << name << "(" << args << ")";
                }
                oss << "\n]";
                content = oss.str();
            }
            parts.push_back("[ASSISTANT]: " + content);
            continue;
        }

        // user / system / other
        if (content.size() > cfg.content_max) {
            content = truncate_middle(content, cfg.content_head, cfg.content_tail);
        }
        parts.push_back("[" + to_upper(role) + "]: " + content);
    }

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += "\n\n";
        out += parts[i];
    }
    return out;
}

SummaryRoleResult pick_summary_role(std::string_view last_head_role,
                                    std::string_view first_tail_role) {
    SummaryRoleResult out;
    std::string role;
    if (last_head_role == "assistant" || last_head_role == "tool") {
        role = "user";
    } else {
        role = "assistant";
    }
    if (role == first_tail_role) {
        std::string flipped = (role == "user") ? "assistant" : "user";
        if (flipped != last_head_role) {
            role = flipped;
        } else {
            out.merge_into_tail = true;
        }
    }
    out.role = role;
    return out;
}

bool can_compress(std::size_t n_messages, int protect_first_n) {
    std::size_t min_for_compress = static_cast<std::size_t>(protect_first_n) + 3 + 1;
    return n_messages > min_for_compress;
}

}  // namespace hermes::agent::compressor_depth
