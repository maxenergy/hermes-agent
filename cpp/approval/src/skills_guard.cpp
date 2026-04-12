#include "hermes/approval/skills_guard.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>

namespace hermes::approval {

namespace {

// The 7 prompt-injection patterns. Mirrored from
// agent::PromptBuilder::is_injection_safe (sibling library — duplicated
// here intentionally, per Phase 6 spec).
const std::vector<std::regex>& injection_patterns() {
    static const std::vector<std::regex> table = [] {
        std::vector<std::regex> v;
        const auto opts = std::regex::ECMAScript | std::regex::icase;
        v.emplace_back(R"(ignore\s+(?:\w+\s+)*(previous|all|above|prior)\s+instructions)", opts);
        v.emplace_back(R"(disregard\s+(?:\w+\s+)*(your|all|any)\s+(?:\w+\s+)*(instructions|rules|guidelines))", opts);
        v.emplace_back(R"(system\s+prompt\s+override)", opts);
        v.emplace_back(R"(do\s+not\s+(?:\w+\s+)*tell\s+(?:\w+\s+)*the\s+user)", opts);
        v.emplace_back(R"(output\s+(?:\w+\s+)*(system|initial)\s+prompt)", opts);
        v.emplace_back(R"(act\s+as\s+(if|though)\s+(?:\w+\s+)*you\s+(?:\w+\s+)*(have\s+no|don'?t\s+have)\s+(?:\w+\s+)*(restrictions|limits|rules))", opts);
        v.emplace_back(R"(<!--[^>]*(?:ignore|override|system|secret|hidden)[^>]*-->)", opts);
        return v;
    }();
    return table;
}

bool is_valid_skill_name(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool path_under_any_root(
    const std::filesystem::path& resolved_path,
    const std::vector<std::filesystem::path>& approved_roots) {
    for (const auto& root : approved_roots) {
        std::error_code ec;
        const auto resolved_root = std::filesystem::weakly_canonical(root, ec);
        const auto rel = std::filesystem::relative(resolved_path, resolved_root, ec);
        if (ec) continue;
        const std::string s = rel.string();
        if (!s.empty() && s.rfind("..", 0) != 0 && s != "..") {
            return true;
        }
    }
    return false;
}

}  // namespace

SkillValidation validate_skill(
    const std::filesystem::path& skill_md_path,
    const std::vector<std::filesystem::path>& approved_roots,
    std::size_t max_bytes) {
    SkillValidation result;
    result.safe = true;

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(skill_md_path, ec);
    if (ec) {
        result.safe = false;
        result.reasons.push_back("path resolution failed: " + ec.message());
        return result;
    }

    if (!std::filesystem::exists(canonical, ec)) {
        result.safe = false;
        result.reasons.push_back("file does not exist");
        return result;
    }
    if (!std::filesystem::is_regular_file(canonical, ec)) {
        result.safe = false;
        result.reasons.push_back("not a regular file");
        return result;
    }

    if (!path_under_any_root(canonical, approved_roots)) {
        result.safe = false;
        result.reasons.push_back("path is not under any approved skills root");
    }

    // Skill name = parent directory name
    const std::string skill_name = canonical.parent_path().filename().string();
    if (!is_valid_skill_name(skill_name)) {
        result.safe = false;
        result.reasons.push_back("skill name '" + skill_name +
                                 "' contains invalid characters");
    }

    // Size cap
    const auto sz = std::filesystem::file_size(canonical, ec);
    if (ec) {
        result.safe = false;
        result.reasons.push_back("could not read file size");
        return result;
    }
    if (sz > max_bytes) {
        result.safe = false;
        result.reasons.push_back("SKILL.md exceeds size cap (" +
                                 std::to_string(sz) + " > " +
                                 std::to_string(max_bytes) + ")");
        return result;
    }

    std::ifstream f(canonical);
    if (!f) {
        result.safe = false;
        result.reasons.push_back("could not open file");
        return result;
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    const std::string body = oss.str();

    // YAML frontmatter sanity: must open with `---\n` and have a closing
    // `---` line within the first ~200 lines. Body without frontmatter is OK.
    if (body.size() >= 4 && body.substr(0, 4) == "---\n") {
        const auto closing = body.find("\n---", 4);
        if (closing == std::string::npos) {
            result.safe = false;
            result.reasons.push_back("YAML frontmatter is missing closing ---");
        }
    }

    // Prompt injection scan
    for (const auto& rx : injection_patterns()) {
        if (std::regex_search(body, rx)) {
            result.safe = false;
            result.reasons.push_back(
                "SKILL.md contains a prompt-injection pattern");
            break;
        }
    }

    return result;
}

}  // namespace hermes::approval
