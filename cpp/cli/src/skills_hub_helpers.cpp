// Pure-logic helpers ported from `hermes_cli/skills_hub.py`.
#include "hermes/cli/skills_hub_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace hermes::cli::skills_hub_helpers {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------

int trust_rank(const std::string& trust_level) {
    if (trust_level == "builtin") {
        return 3;
    }
    if (trust_level == "trusted") {
        return 2;
    }
    if (trust_level == "community") {
        return 1;
    }
    return 0;
}

std::string trust_style(const std::string& trust_level) {
    if (trust_level == "builtin") {
        return "bright_cyan";
    }
    if (trust_level == "trusted") {
        return "green";
    }
    if (trust_level == "community") {
        return "yellow";
    }
    return "dim";
}

std::string trust_label(const std::string& source,
                        const std::string& trust_level) {
    if (source == "official") {
        return "official";
    }
    return trust_level;
}

// ---------------------------------------------------------------------------

std::string derive_category_from_install_path(
    const std::string& install_path) {
    if (install_path.empty()) {
        return "";
    }
    const std::filesystem::path p {install_path};
    const std::string parent {p.parent_path().generic_string()};
    if (parent.empty() || parent == ".") {
        return "";
    }
    return parent;
}

// ---------------------------------------------------------------------------

int per_source_limit(const std::string& source) {
    static const std::unordered_map<std::string, int> limits {
        {"official", 200},
        {"skills-sh", 200},
        {"well-known", 50},
        {"github", 200},
        {"clawhub", 500},
        {"claude-marketplace", 100},
        {"lobehub", 500},
    };
    const auto it {limits.find(source)};
    if (it != limits.end()) {
        return it->second;
    }
    return 100;
}

// ---------------------------------------------------------------------------

PageWindow paginate(std::size_t total,
                    std::size_t page,
                    std::size_t page_size) {
    PageWindow w {};
    w.page_size = std::max<std::size_t>(1, std::min<std::size_t>(100, page_size));
    if (total == 0) {
        w.total_pages = 1;
        w.page = 1;
        w.start = 0;
        w.end = 0;
        return w;
    }
    w.total_pages = std::max<std::size_t>(
        1, (total + w.page_size - 1) / w.page_size);
    w.page = std::max<std::size_t>(
        1, std::min<std::size_t>(page, w.total_pages));
    w.start = (w.page - 1) * w.page_size;
    w.end = std::min<std::size_t>(w.start + w.page_size, total);
    return w;
}

// ---------------------------------------------------------------------------

std::vector<std::string> format_extra_metadata_lines(
    const ExtraMetadata& extra) {
    std::vector<std::string> lines;
    if (!extra.repo_url.empty()) {
        lines.push_back("[bold]Repo:[/] " + extra.repo_url);
    }
    if (!extra.detail_url.empty()) {
        lines.push_back("[bold]Detail Page:[/] " + extra.detail_url);
    }
    if (!extra.index_url.empty()) {
        lines.push_back("[bold]Index:[/] " + extra.index_url);
    }
    if (!extra.endpoint.empty()) {
        lines.push_back("[bold]Endpoint:[/] " + extra.endpoint);
    }
    if (!extra.install_command.empty()) {
        lines.push_back("[bold]Install Command:[/] " + extra.install_command);
    }
    if (extra.installs.has_value()) {
        lines.push_back("[bold]Installs:[/] " + std::to_string(*extra.installs));
    }
    if (!extra.weekly_installs.empty()) {
        lines.push_back("[bold]Weekly Installs:[/] " + extra.weekly_installs);
    }
    if (!extra.security_audits.empty()) {
        std::vector<std::pair<std::string, std::string>> sorted_audits {
            extra.security_audits};
        std::sort(sorted_audits.begin(), sorted_audits.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        std::string joined;
        for (std::size_t i {0}; i < sorted_audits.size(); ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += sorted_audits[i].first;
            joined += "=";
            joined += sorted_audits[i].second;
        }
        lines.push_back("[bold]Security:[/] " + joined);
    }
    return lines;
}

// ---------------------------------------------------------------------------

std::vector<Result> deduplicate_by_name(
    const std::vector<Result>& results) {
    // Preserve insertion order: first occurrence wins unless a later one
    // has strictly higher trust rank.
    std::unordered_map<std::string, std::size_t> index_by_name;
    std::vector<Result> out;
    out.reserve(results.size());
    for (const Result& r : results) {
        const auto it {index_by_name.find(r.name)};
        if (it == index_by_name.end()) {
            index_by_name[r.name] = out.size();
            out.push_back(r);
        } else {
            const int existing_rank {trust_rank(out[it->second].trust_level)};
            if (trust_rank(r.trust_level) > existing_rank) {
                out[it->second] = r;
            }
        }
    }
    return out;
}

void sort_browse_results(std::vector<Result>& results) {
    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b) {
                  const int ra {trust_rank(a.trust_level)};
                  const int rb {trust_rank(b.trust_level)};
                  if (ra != rb) {
                      return ra > rb;
                  }
                  const bool ao {a.source == "official"};
                  const bool bo {b.source == "official"};
                  if (ao != bo) {
                      return ao;
                  }
                  return to_lower(a.name) < to_lower(b.name);
              });
}

// ---------------------------------------------------------------------------

ResolveOutcome classify_resolution(
    const std::vector<Result>& exact_matches) {
    if (exact_matches.empty()) {
        return ResolveOutcome::NoMatch;
    }
    if (exact_matches.size() == 1) {
        return ResolveOutcome::Exact;
    }
    return ResolveOutcome::Ambiguous;
}

// ---------------------------------------------------------------------------

std::string truncate_description(const std::string& desc, std::size_t max_len) {
    if (desc.size() <= max_len) {
        return desc;
    }
    return desc.substr(0, max_len) + "...";
}

// ---------------------------------------------------------------------------

bool is_valid_tap_action(const std::string& action) {
    static const std::unordered_set<std::string> actions {
        "add", "remove", "list", "update",
    };
    return actions.count(action) > 0;
}

std::string source_label(const std::string& source) {
    if (source == "official") {
        return "Nous Research (official)";
    }
    if (source == "skills-sh") {
        return "skills.sh";
    }
    if (source == "github") {
        return "GitHub";
    }
    if (source == "clawhub") {
        return "ClawHub";
    }
    if (source == "lobehub") {
        return "LobeHub";
    }
    if (source == "well-known") {
        return ".well-known";
    }
    if (source == "claude-marketplace") {
        return "Claude Marketplace";
    }
    return source;
}

bool is_valid_source_filter(const std::string& source) {
    if (source == "all") {
        return true;
    }
    static const std::unordered_set<std::string> known {
        "official", "skills-sh", "well-known", "github",
        "clawhub", "claude-marketplace", "lobehub",
    };
    return known.count(source) > 0;
}

// ---------------------------------------------------------------------------

std::string search_header(std::size_t count) {
    return "Skills Hub \xE2\x80\x94 " + std::to_string(count) + " result(s)";
}

std::string browse_status_line(std::size_t total,
                               std::size_t page,
                               std::size_t total_pages,
                               const std::string& source_filter,
                               std::size_t timed_out_sources) {
    std::string out {"Skills Hub \xE2\x80\x94 Browse "};
    if (source_filter == "all" || source_filter.empty()) {
        out += "\xE2\x80\x94 all sources";
    } else {
        out += "\xE2\x80\x94 ";
        out += source_filter;
    }
    out += "  (";
    out += std::to_string(total);
    out += " skills loaded";
    if (timed_out_sources > 0) {
        out += ", ";
        out += std::to_string(timed_out_sources);
        out += " source(s) still loading";
    }
    out += ", page ";
    out += std::to_string(page);
    out += "/";
    out += std::to_string(total_pages);
    out += ")";
    return out;
}

}  // namespace hermes::cli::skills_hub_helpers
